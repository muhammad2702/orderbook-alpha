// ============================================================================
//  MM-ALPHA  —  Market-Maker Behaviour Analytics on IBKR Level-2
// ============================================================================
//
//  WHAT THIS IS
//  ------------
//  A research terminal for extracting *directional* information from the
//  identities behind quotes. Not an execution-cost tool.
//
//  THE CENTRAL BUG THAT WAS FIXED
//  ------------------------------
//  IBKR L2 (`updateMktDepthL2`) is a POSITION-INDEXED protocol:
//      operation 0 = insert a new row at `position` (rows below shift down)
//      operation 1 = update the row at `position`
//      operation 2 = delete the row at `position` (rows below shift up)
//
//  The identity (MPID) of a row that is being DELETED lives in *our* ladder at
//  that position. IBKR routinely sends operation=2 with an EMPTY marketMaker
//  and a stale/zero price. The previous code keyed market-maker state by
//  (name + price + side + venue), so a delete synthesised a DIFFERENT key than
//  the insert had ("UNKNOWN_0_1_0"). Consequences:
//      * the real entry never died      -> phantom permanent MM lines
//      * UNKNOWN_* keys accumulated     -> "everything drops for that key"
//  Second, independent cause: `int(price*100)` truncates. 227.50*100 is
//  22749.9999... in IEEE double -> 22749. The same price randomly split across
//  two keys.
//
//  FIX: model the book as the protocol defines it (Ladder, below). A cancel
//  reads its MPID/price/size from the slot being removed. Prices are keyed by
//  llround(px*10000). "Unattributed" is now a first-class identity (ANON),
//  never a bucket for lost information.
//
//  LAYERING (data flows one way, no back-edges)
//  --------------------------------------------
//      Ingest (IB callbacks / Sim)
//        -> Ladder            protocol-correct book, per stream
//        -> Event             {ts, stream, mm, side, kind, px, sz}  (normalised)
//        -> Recorder          CSV to disk (research/replay)
//        -> Analytics         profiles, spoof, markout, lead-lag  (no locks)
//        -> Signal            composite + LIVE forward-return scoreboard
//        -> UI
//
//  THREADING: EReader::processMsgs() dispatches callbacks on the thread that
//  calls it — here, the UI thread. So ingest and analytics are single-threaded
//  and require NO locks. This is a deliberate simplification, not an oversight.
//
// ============================================================================

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <GLFW/glfw3.h>
#include <imgui.h>
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <DefaultEWrapper.h>
#include <EReaderSignal.h>
#include <EReaderOSSignal.h>
#include "EClientSocket.h"
#include "EReader.h"
#include "Contract.h"
#include "TagValue.h"
#include "ib_wrapper.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

// ============================================================================
//  0. SMALL UTILITIES
// ============================================================================

static int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static inline double Clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Price key: exact to 1/100 of a cent. Never use int(px*100).
static inline int64_t PxKey(double px) { return (int64_t)llround(px * 10000.0); }

// Exponentially weighted mean. `alpha` is the weight of each new sample.
struct Ewma
{
    double v = 0.0;
    bool   primed = false;
    double alpha;

    explicit Ewma(double a = 0.05) : alpha(a) {}

    void Push(double x)
    {
        if (!primed) { v = x; primed = true; }
        else         { v += alpha * (x - v); }
    }
};

// Running mean / variance (Welford). Used wherever we quote a stable average.
struct RunStat
{
    int64_t n = 0;
    double  mean = 0.0;
    double  m2 = 0.0;

    void Push(double x)
    {
        ++n;
        double d = x - mean;
        mean += d / (double)n;
        m2 += d * (x - mean);
    }
    double Var() const { return n > 1 ? m2 / (double)(n - 1) : 0.0; }
    double Sd()  const { return std::sqrt(Var()); }
    double StdErr() const { return n > 1 ? Sd() / std::sqrt((double)n) : 0.0; }
    // t-statistic of the mean against zero. |t| > 2 is the usual bar.
    double T() const { double se = StdErr(); return se > 1e-12 ? mean / se : 0.0; }
    void Reset() { n = 0; mean = 0.0; m2 = 0.0; }
};

// Fixed-capacity float ring, oldest-to-newest iteration by index.
template <int N>
struct Ring
{
    float   buf[N] = { 0 };
    int     head = 0;      // index of NEXT write
    int     count = 0;

    void Push(float x)
    {
        buf[head] = x;
        head = (head + 1) % N;
        if (count < N) ++count;
    }
    // i = 0 is the oldest retained sample.
    float At(int i) const { return buf[(head - count + i + 2 * N) % N]; }
    float Last() const { return count ? buf[(head - 1 + N) % N] : 0.0f; }
    void Clear() { head = 0; count = 0; }
};

// Pearson correlation of x[t] against y[t+lag], over the overlapping region.
template <int N>
static double LaggedCorr(const Ring<N>& x, const Ring<N>& y, int lag)
{
    int n = std::min(x.count, y.count) - lag;
    if (n < 30) return 0.0;

    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; ++i)
    {
        double a = x.At(i);
        double b = y.At(i + lag);
        sx += a; sy += b; sxx += a * a; syy += b * b; sxy += a * b;
    }
    double dn = (double)n;
    double cov = sxy / dn - (sx / dn) * (sy / dn);
    double vx = sxx / dn - (sx / dn) * (sx / dn);
    double vy = syy / dn - (sy / dn) * (sy / dn);
    if (vx <= 1e-18 || vy <= 1e-18) return 0.0;
    return cov / std::sqrt(vx * vy);
}

// ============================================================================
//  1. IDENTITY TABLE  (MPID interning)
// ============================================================================
//
//  MPIDs are interned to uint16 so that every downstream structure is a flat
//  array indexed by id — no string hashing in the hot path.
//
//  Id 0 is reserved for ANON: a quote the venue chose not to attribute. On
//  NASDAQ TotalView the venue-aggregate arrives explicitly as "NSDQ"; on
//  price-aggregated feeds (ARCA/BATS/IEX) the field is empty. Both are
//  "not a named firm", but they are NOT the same thing, so we keep them apart.
// ============================================================================

static const int kMaxMM = 512;

struct MMTable
{
    std::vector<std::string>                     names;
    std::unordered_map<std::string, uint16_t>    index;

    MMTable()
    {
        names.reserve(64);
        Intern("ANON");    // id 0 — empty marketMaker field
    }

    uint16_t Intern(const std::string& raw)
    {
        const std::string s = raw.empty() ? std::string("ANON") : raw;
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        if ((int)names.size() >= kMaxMM) return 0;
        uint16_t id = (uint16_t)names.size();
        names.push_back(s);
        index[s] = id;
        return id;
    }

    const char* Name(uint16_t id) const
    {
        return id < names.size() ? names[id].c_str() : "?";
    }
    int Count() const { return (int)names.size(); }
};

static MMTable g_mm;

// "Is this identity a real, named firm?"  ANON and the venue aggregates are not.
static bool IsAttributed(uint16_t id)
{
    if (id == 0) return false;
    const char* n = g_mm.Name(id);
    return !(strcmp(n, "NSDQ") == 0 || strcmp(n, "ARCA") == 0 ||
             strcmp(n, "BATS") == 0 || strcmp(n, "IEX") == 0 ||
             strcmp(n, "EDGX") == 0 || strcmp(n, "BYX") == 0);
}

// ============================================================================
//  2. NORMALISED EVENT STREAM
// ============================================================================
//
//  Everything downstream — recorder, analytics, UI — consumes only this.
//  That is what makes live capture and file replay literally the same code.
// ============================================================================

enum class EvKind : uint8_t { Add = 0, Modify = 1, Cancel = 2, Fill = 3, Trade = 4 };

static const char* KindName(EvKind k)
{
    switch (k) {
    case EvKind::Add:    return "ADD";
    case EvKind::Modify: return "MOD";
    case EvKind::Cancel: return "CXL";
    case EvKind::Fill:   return "FILL";
    default:             return "TRADE";
    }
}

struct Event
{
    int64_t  ts = 0;         // ns, steady clock
    uint16_t stream = 0;
    uint16_t mm = 0;
    uint8_t  side = 0;       // 1 = bid, 0 = ask.  For Trade: aggressor side.
    EvKind   kind = EvKind::Add;
    double   px = 0.0;
    int32_t  sz = 0;         // Add/Modify: new resting size. Cancel/Fill: size removed.
    int32_t  prevSz = 0;
    int64_t  lifeNs = 0;     // Cancel/Fill only: how long the quote rested.
    int32_t  depth = 0;      // ticks away from that side's touch at event time
};

// ============================================================================
//  3. STREAM REGISTRY  (symbol x venue)
// ============================================================================
//
//  IBKR NOTE — READ THIS BEFORE ADDING STREAMS:
//  A retail account gets a small number of *simultaneous* market-depth lines
//  (commonly 3; more only if you have purchased extra quote lines). Requesting
//  more returns error 309 "max number of market depth requests exceeded".
//  We surface that error verbatim in the Streams panel rather than failing
//  silently. `kRotateStreams` can time-slice a larger universe, but rotation
//  destroys continuous per-MM tracking, so it is OFF by default.
// ============================================================================

static const int kMaxStreams = 12;
static const int kMaxLadder = 64;      // rows per side we are willing to hold

struct StreamCfg
{
    char   symbol[16] = "ALLW";
    char   venue[16] = "ISLAND";     // IBKR depth-exchange code
    char   primary[16] = "NASDAQ";   // listing exchange, disambiguates the contract
    double tick = 0.01;
    bool   enabled = false;
};

// ---------------------------------------------------------------------------
//  3a. LADDER — the protocol-correct book. This is the bug fix.
// ---------------------------------------------------------------------------

struct Slot
{
    double   px = 0.0;
    int32_t  sz = 0;
    uint16_t mm = 0;
    int64_t  birth = 0;    // when this slot's order first appeared
    int64_t  touched = 0;  // last modification
};

struct SideLadder
{
    Slot s[kMaxLadder];
    int  n = 0;

    void InsertAt(int pos, const Slot& v)
    {
        if (pos < 0 || pos >= kMaxLadder) return;
        int last = std::min(n, kMaxLadder - 1);
        for (int i = last; i > pos; --i) s[i] = s[i - 1];
        s[pos] = v;
        if (n < kMaxLadder) ++n;
    }

    // Returns the slot that was removed, so the caller knows WHO cancelled.
    bool EraseAt(int pos, Slot& out)
    {
        if (pos < 0 || pos >= n) return false;
        out = s[pos];
        for (int i = pos; i < n - 1; ++i) s[i] = s[i + 1];
        s[n - 1] = Slot{};
        --n;
        return true;
    }

    double Touch() const { return n > 0 ? s[0].px : 0.0; }
};

// ============================================================================
//  4. PER-MARKET-MAKER PROFILE
// ============================================================================
//
//  One of these per (stream, mpid). Every number here is meant to answer a
//  single question: "does watching this participant tell me where price goes?"
// ============================================================================

static const int kLifeBuckets = 10;
// The top edge is finite on purpose: an unbounded bucket would poison the
// median that the fingerprint metric depends on.
static const double kLifeEdgesMs[kLifeBuckets] =
{ 1, 10, 50, 100, 250, 500, 1000, 5000, 30000, 300000 };

static const int kFairSeries = 3000;   // 100ms samples -> 5 minutes

struct MMProfile
{
    bool     seen = false;
    int64_t  firstTs = 0;
    int64_t  lastTs = 0;

    // ---- live quoting state (recomputed from the ladder on a 100ms cadence)
    double   bestBid = 0.0, bestAsk = 0.0;
    int32_t  bidSz = 0, askSz = 0;
    double   fair = 0.0;      // (bid+ask)/2 while two-sided — the reservation price
    double   fairPrev = 0.0;
    double   skew = 0.0;      // (bidSz-askSz)/(bidSz+askSz): inventory lean
    bool     twoSided = false;
    int64_t  twoSidedSamples = 0, totalSamples = 0;

    // ---- flow counters
    int64_t  adds = 0, mods = 0, cancels = 0, fills = 0;
    int64_t  szAdded = 0, szCancelled = 0, szFilled = 0;

    // ---- quote lifetime
    int64_t  lifeHist[kLifeBuckets] = { 0 };
    RunStat  lifeMs;
    int64_t  subs100 = 0, lifeSamples = 0;

    // ---- replenishment / iceberg
    int      refills = 0;
    RunStat  refillLatMs;
    std::unordered_map<int64_t, int64_t> lastDeathAtPx;   // pxkey -> ts

    // ---- competition / initiative
    int      initiations = 0;   // established a NEW best price
    int      joins = 0;         // added at the existing best
    int      fades = 0;         // pulled the best and worsened it
    RunStat  initEdge;          // mid move (bps) 2s after this MM initiated

    // ---- adverse selection (needs the trade tape)
    RunStat  markout1s;         // + = MM made money on the fill (aggressor uninformed)
    RunStat  markout5s;         // - = MM run over  (aggressor informed)  <-- the signal

    // ---- manipulation
    int      flickers = 0;      // add->cancel at same px, life < 100ms
    int      spoofFlags = 0;
    double   spoofScore = 0.0;

    // ---- size quantisation (algo fingerprint)
    int64_t  roundLots = 0, oddLots = 0;
    RunStat  addSize;
    RunStat  depthTicks;        // how far from touch it likes to sit
    RunStat  gapMs;             // inter-event arrival -> cadence
    int64_t  lastEventTs = 0;

    // ---- predictive scoring
    Ring<kFairSeries> dFair;    // per-100ms change in this MM's fair value
    double   ic[4] = { 0,0,0,0 };  // corr(dFair[t], dMid[t+lag]) for lags below
    double   bestIC = 0.0;
    int      bestLag = 0;

    double CancelRatio() const
    {
        int64_t d = adds > 0 ? adds : 1;
        return (double)cancels / (double)d;
    }
    double FillRatio() const
    {
        int64_t denom = szCancelled + szFilled;
        return denom > 0 ? (double)szFilled / (double)denom : 0.0;
    }
    double MedianLifeMs() const
    {
        int64_t tot = 0;
        for (int i = 0; i < kLifeBuckets; ++i) tot += lifeHist[i];
        if (tot == 0) return 0.0;
        int64_t half = tot / 2, run = 0;
        for (int i = 0; i < kLifeBuckets; ++i)
        {
            run += lifeHist[i];
            if (run >= half) return kLifeEdgesMs[i];
        }
        return 0.0;
    }
    double SubSecondFrac() const
    {
        return lifeSamples > 0 ? (double)subs100 / (double)lifeSamples : 0.0;
    }
    double TwoSidedFrac() const
    {
        return totalSamples > 0 ? (double)twoSidedSamples / (double)totalSamples : 0.0;
    }
};

// Lags (in 100ms samples) at which we test whether an MM leads the mid.
static const int kICLags[4] = { 1, 5, 10, 30 };   // 0.1s, 0.5s, 1s, 3s

// ============================================================================
//  5. MANIPULATION + MARKOUT BOOKKEEPING
// ============================================================================

struct SpoofEvent
{
    int64_t  ts = 0;
    uint16_t stream = 0;
    uint16_t mm = 0;
    uint8_t  side = 0;
    double   px = 0.0;
    int32_t  sz = 0;
    double   lifeMs = 0.0;
    // components, all 0..1, kept separately so a flag is always explainable
    double   cSize = 0, cDepth = 0, cLife = 0, cImbal = 0, cFollow = 0;
    double   score = 0.0;
    bool     confirmed = false;   // opposite-side aggression actually followed
};

// A fill we are waiting to score. Resolved at +1s and +5s.
struct PendingMarkout
{
    int64_t  ts = 0;
    uint16_t stream = 0;
    uint16_t mm = 0;
    uint8_t  side = 0;
    double   px = 0.0;
    double   midAt = 0.0;
    int32_t  sz = 0;
    bool     done1 = false;
};

struct TradePrint
{
    int64_t ts = 0;
    double  px = 0.0;
    int32_t sz = 0;
    uint8_t aggressor = 0;   // 1 = buyer lifted the ask, 0 = seller hit the bid
};

// ============================================================================
//  6. HEATMAP GRID
// ============================================================================
//
//  A bookmap does not need full order-book snapshots — it needs a
//  time x price raster. Storing the raster instead of 100k snapshots cuts
//  memory ~100x (1.1 MB/stream vs ~160 MB/stream) and draws far faster.
// ============================================================================

static const int kGridCols = 2880;    // x 250ms = 12 minutes
static const int kGridRows = 192;
static const int64_t kGridDtNs = 250ll * 1000000ll;

struct HeatGrid
{
    uint16_t bid[kGridCols][kGridRows] = { {0} };
    uint16_t ask[kGridCols][kGridRows] = { {0} };
    float    mid[kGridCols] = { 0 };
    uint16_t colMax[kGridCols] = { 0 };   // per-column peak, so the renderer
                                          // never has to rescan 550k cells
    double   p0 = 0.0;          // price at row 0
    double   tick = 0.01;
    int      head = 0;
    int      count = 0;
    int64_t  lastCol = 0;

    int RowOf(double px) const
    {
        if (p0 <= 0.0) return -1;
        int r = (int)llround((px - p0) / tick);
        return (r >= 0 && r < kGridRows) ? r : -1;
    }
    void Recenter(double mid_)
    {
        p0 = mid_ - (kGridRows / 2) * tick;
    }
};

// ============================================================================
//  7. STREAM STATE  (everything about one symbol on one venue)
// ============================================================================

struct Stream
{
    StreamCfg cfg;
    int       id = 0;
    bool      live = false;
    std::string lastError;

    // --- book
    SideLadder bids, asks;
    double     bestBid = 0.0, bestAsk = 0.0;
    double     mid = 0.0, prevMid = 0.0;
    int64_t    lastUpdate = 0;

    // --- throughput
    int64_t    evCount = 0;
    Ewma       evRate{ 0.02 };
    int64_t    rateWindowStart = 0;
    int64_t    rateWindowCount = 0;

    // --- per-MM
    std::vector<MMProfile> prof;   // indexed by mm id

    // --- trade tape (recent prints, for cancel-vs-fill classification)
    std::deque<TradePrint> tape;
    int64_t   tradeCount = 0;
    int64_t   buyVol = 0, sellVol = 0;

    // --- markout queues. Two of them, each time-ordered, each popped from the
    //     front, so a horizon is never scored twice and never skipped.
    std::deque<PendingMarkout> pend1s;
    std::deque<PendingMarkout> pend5s;
    // --- "did this MM's price improvement lead anywhere?" resolved at +2s
    std::deque<PendingMarkout> pendInit;

    // --- manipulation log
    std::deque<SpoofEvent> spoofLog;

    // --- sampled series (100ms cadence)
    Ring<kFairSeries> dMid;
    Ring<kFairSeries> midSeries;
    int64_t   lastSample = 0;

    // --- book-level aggregates
    double    imbalance = 0.0;       // touch-size imbalance, -1..1
    double    imbalanceClean = 0.0;  // with flagged spoof size removed
    double    cancelAsym = 0.0;      // EWMA of (bidCancelVol - askCancelVol) normalised
    Ewma      bidCxlVol{ 0.02 }, askCxlVol{ 0.02 };

    // --- signal
    double    signal = 0.0;
    Ring<kFairSeries> sigSeries;

    // --- heatmap
    HeatGrid  grid;

    // Pre-sized to kMaxMM at configuration time and never resized, so a
    // reference handed out here can never be invalidated by a later intern.
    MMProfile& Prof(uint16_t mmId)
    {
        return prof[mmId < prof.size() ? mmId : 0];
    }
};

static Stream   g_streams[kMaxStreams];
static int      g_streamCount = 0;

// Cached cross-venue lead-lag: [leader][follower]. Each cell is O(3000) to
// compute, so it is refreshed once per second rather than once per frame.
static double   g_llCorr[kMaxStreams][kMaxStreams] = { {0} };
static int      g_llLag[kMaxStreams][kMaxStreams] = { {0} };

// ============================================================================
//  8. LIVE SIGNAL EVALUATION
// ============================================================================
//
//  The single most important panel in this program. Any dashboard can draw a
//  confident-looking number; this records the number alongside the realised
//  forward return and shows the hit rate accumulating in real time. If the
//  t-stat here does not clear ~2, nothing else on screen is tradeable.
// ============================================================================

//  Sampling is NON-OVERLAPPING on purpose. Logging a fresh observation every
//  100ms would produce heavily autocorrelated samples whose t-stat is inflated
//  by roughly sqrt(overlap) - it would show a t of 6 on pure noise. So the 5s
//  horizon is sampled once per 5s and the 30s horizon once per 30s, each in its
//  own queue. Fewer observations, but the number means what it says.
struct SignalEval
{
    struct Pending { int64_t ts; int stream; double sig; double mid; };
    std::deque<Pending> q5, q30;
    int64_t  lastPush5 = 0, lastPush30 = 0;

    RunStat  ret5s, ret30s;      // signed by the signal -> mean > 0 means it works
    int64_t  hits5 = 0, tot5 = 0;
    int64_t  hits30 = 0, tot30 = 0;

    double HitRate5()  const { return tot5 ? (double)hits5 / (double)tot5 : 0.0; }
    double HitRate30() const { return tot30 ? (double)hits30 / (double)tot30 : 0.0; }
};

static SignalEval g_eval;

// ============================================================================
//  9. GLOBAL CONFIG (UI-editable)
// ============================================================================

static bool   g_simMode = false;      // synthetic feed, for testing while closed
static bool   g_recording = false;
static FILE*  g_recFile = nullptr;
static int64_t g_recLines = 0;

static bool   g_showHeatmap = true;
static bool   g_showMMLines = true;
static bool   g_showMid = true;
static float  g_heatGain = 1.0f;
static int    g_selStream = 0;

static float  g_spoofThreshold = 0.60f;
static float  g_minMMSize = 1.0f;
static bool   g_hideAnon = false;
static float  g_flickerMs = 100.0f;
static float  g_refillWindowMs = 2000.0f;

// Signal component weights. Exposed because the honest way to use this is to
// watch the evaluation panel while you change them.
static float  g_wFair = 1.0f;    // MM fair-value drift, IC-weighted
static float  g_wImbal = 0.6f;   // spoof-cleaned touch imbalance
static float  g_wCxl = 0.5f;     // cancellation asymmetry
static float  g_wMarkout = 0.8f; // adverse-selection direction
static float  g_wLead = 0.4f;    // cross-venue lead


// ============================================================================
//  10. RECORDER
// ============================================================================

static void RecOpen()
{
    if (g_recFile) return;
    char path[256];
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    snprintf(path, sizeof(path), "mmalpha_%04d%02d%02d_%02d%02d%02d.csv",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    g_recFile = fopen(path, "wb");
    if (g_recFile)
    {
        setvbuf(g_recFile, nullptr, _IOFBF, 1 << 20);
        fprintf(g_recFile, "ts_ns,symbol,venue,mm,side,kind,px,sz,prev_sz,life_ms,depth_ticks\n");
        g_recLines = 0;
        g_recording = true;
    }
}

static void RecClose()
{
    if (g_recFile) { fclose(g_recFile); g_recFile = nullptr; }
    g_recording = false;
}

static void RecWrite(const Event& e)
{
    if (!g_recFile) return;
    const Stream& s = g_streams[e.stream];
    fprintf(g_recFile, "%lld,%s,%s,%s,%d,%s,%.4f,%d,%d,%.3f,%d\n",
        (long long)e.ts, s.cfg.symbol, s.cfg.venue, g_mm.Name(e.mm),
        (int)e.side, KindName(e.kind), e.px, e.sz, e.prevSz,
        e.lifeNs / 1e6, e.depth);
    ++g_recLines;
}

// ============================================================================
//  11. ANALYTICS — event consumption
// ============================================================================

// Recompute the touch AND attribute the change to whoever caused it.
//
// This is the "competition between market makers" measurement. A participant
// that repeatedly establishes a NEW best price is revealing a view; one that
// only ever joins an existing best is following. Whether that revelation is
// worth anything is answered by initEdge, resolved 2s later.
//
// NOTE: prevMid is deliberately NOT touched here. It is owned by the 100ms
// sampler, so that dMid is a 100ms return and not a per-message return.
static void RecomputeTouch(Stream& s, uint16_t actor, EvKind kind, int64_t ts)
{
    double oldBid = s.bestBid, oldAsk = s.bestAsk;

    s.bestBid = s.bids.Touch();
    s.bestAsk = s.asks.Touch();
    if (s.bestBid > 0.0 && s.bestAsk > 0.0 && s.bestAsk >= s.bestBid)
        s.mid = 0.5 * (s.bestBid + s.bestAsk);

    if (oldBid <= 0.0 && oldAsk <= 0.0) return;      // book still warming up
    MMProfile& p = s.Prof(actor);

    int dir = 0;   // +1 bullish improvement, -1 bearish improvement
    if (kind == EvKind::Add)
    {
        if (oldBid > 0.0 && s.bestBid > oldBid) { ++p.initiations; dir = +1; }
        else if (oldAsk > 0.0 && s.bestAsk > 0.0 && s.bestAsk < oldAsk) { ++p.initiations; dir = -1; }
        else if (PxKey(s.bestBid) == PxKey(oldBid) || PxKey(s.bestAsk) == PxKey(oldAsk)) ++p.joins;
    }
    else if (kind == EvKind::Cancel)
    {
        if ((oldBid > 0.0 && s.bestBid < oldBid) ||
            (oldAsk > 0.0 && s.bestAsk > oldAsk)) ++p.fades;
    }

    if (dir != 0 && s.mid > 0.0)
    {
        PendingMarkout pi;
        pi.ts = ts; pi.stream = (uint16_t)s.id; pi.mm = actor;
        pi.side = (dir > 0) ? 1 : 0;
        pi.midAt = s.mid; pi.px = s.mid;
        s.pendInit.push_back(pi);
        if (s.pendInit.size() > 4000) s.pendInit.pop_front();
    }
}

// Was this size reduction a trade, or a cancel? Without the tape you cannot
// tell, and every "cancellation" statistic becomes noise. We look for a print
// at the same price within a short window whose size can account for the drop.
static bool LooksLikeFill(Stream& s, int64_t ts, double px, int32_t szRemoved)
{
    const int64_t kWin = 60ll * 1000000ll;   // 60 ms
    for (auto it = s.tape.rbegin(); it != s.tape.rend(); ++it)
    {
        if (ts - it->ts > kWin) break;
        if (std::llabs(PxKey(it->px) - PxKey(px)) <= 1 && it->sz >= szRemoved / 2)
            return true;
    }
    return false;
}

static void ScoreSpoof(Stream& s, const Event& e, MMProfile& p);

// Bucket a quote lifetime. Anything above the top edge lands in the top
// bucket rather than being silently discarded.
static void PushLifeSample(MMProfile& p, double lifeMs)
{
    p.lifeMs.Push(lifeMs);
    ++p.lifeSamples;
    int b = kLifeBuckets - 1;
    for (int i = 0; i < kLifeBuckets; ++i)
        if (lifeMs < kLifeEdgesMs[i]) { b = i; break; }
    ++p.lifeHist[b];
}

static void OnEvent(Stream& s, Event& e)
{
    ++s.evCount;
    ++s.rateWindowCount;

    MMProfile& p = s.Prof(e.mm);
    if (!p.seen) { p.seen = true; p.firstTs = e.ts; }
    p.lastTs = e.ts;

    if (p.lastEventTs) p.gapMs.Push((double)(e.ts - p.lastEventTs) / 1e6);
    p.lastEventTs = e.ts;

    switch (e.kind)
    {
    case EvKind::Add:
    {
        ++p.adds;
        p.szAdded += e.sz;
        p.addSize.Push((double)e.sz);
        p.depthTicks.Push((double)e.depth);
        if (e.sz % 100 == 0) ++p.roundLots; else ++p.oddLots;

        // Replenishment: did this MM just die at this exact price?
        auto it = p.lastDeathAtPx.find(PxKey(e.px));
        if (it != p.lastDeathAtPx.end())
        {
            double dt = (double)(e.ts - it->second) / 1e6;
            if (dt <= g_refillWindowMs)
            {
                ++p.refills;
                p.refillLatMs.Push(dt);
            }
            p.lastDeathAtPx.erase(it);
        }
        break;
    }

    case EvKind::Modify:
        ++p.mods;
        if (e.sz > e.prevSz) p.szAdded += (e.sz - e.prevSz);
        break;

    case EvKind::Cancel:
    {
        ++p.cancels;
        p.szCancelled += e.sz;
        p.lastDeathAtPx[PxKey(e.px)] = e.ts;

        double lifeMs = (double)e.lifeNs / 1e6;
        PushLifeSample(p, lifeMs);
        if (lifeMs < g_flickerMs) { ++p.subs100; ++p.flickers; }

        if (e.side == 1) s.bidCxlVol.Push((double)e.sz); else s.askCxlVol.Push((double)e.sz);
        ScoreSpoof(s, e, p);
        break;
    }

    case EvKind::Fill:
    {
        ++p.fills;
        p.szFilled += e.sz;

        PushLifeSample(p, (double)e.lifeNs / 1e6);

        // Queue an adverse-selection measurement on this fill.
        if (s.mid > 0.0)
        {
            PendingMarkout pm;
            pm.ts = e.ts; pm.stream = (uint16_t)s.id; pm.mm = e.mm;
            pm.side = e.side; pm.px = e.px; pm.midAt = s.mid; pm.sz = e.sz;
            s.pend1s.push_back(pm);
            s.pend5s.push_back(pm);
            if (s.pend1s.size() > 8000) s.pend1s.pop_front();
            if (s.pend5s.size() > 8000) s.pend5s.pop_front();
        }
        break;
    }

    default: break;
    }

    if (g_recording) RecWrite(e);
}

// ---------------------------------------------------------------------------
//  Spoof / layering score.
//
//  Deliberately CONJUNCTIVE and fully decomposed. Large-and-cancelled is not
//  spoofing — that is ordinary market making. What separates the two is the
//  combination: unusually large, posted AWAY from the touch, pulled fast,
//  skewing the visible book, and then followed by aggression on the OTHER
//  side. Every component is stored so any flag can be argued with.
// ---------------------------------------------------------------------------
static void ScoreSpoof(Stream& s, const Event& e, MMProfile& p)
{
    if (s.mid <= 0.0) return;

    double avgAdd = p.addSize.n > 20 ? p.addSize.mean : 0.0;
    if (avgAdd < 1.0) return;

    SpoofEvent sp;
    sp.ts = e.ts; sp.stream = (uint16_t)s.id; sp.mm = e.mm;
    sp.side = e.side; sp.px = e.px; sp.sz = e.sz;
    sp.lifeMs = (double)e.lifeNs / 1e6;

    // 1. Size outlier relative to this MM's own norm.
    sp.cSize = Clamp(((double)e.sz / avgAdd - 1.5) / 3.0, 0.0, 1.0);

    // 2. Sitting away from the touch — a genuine quote wants the queue.
    sp.cDepth = Clamp(((double)e.depth - 1.0) / 8.0, 0.0, 1.0);

    // 3. Short-lived and cancelled (this branch is a cancel, not a fill).
    sp.cLife = Clamp((2000.0 - sp.lifeMs) / 2000.0, 0.0, 1.0);

    // 4. Did it meaningfully skew the visible book while it was there?
    sp.cImbal = Clamp(std::fabs(s.imbalance), 0.0, 1.0);

    // 5. Confirmation comes later (see ResolveSpoofFollow).
    sp.cFollow = 0.0;

    sp.score = 0.30 * sp.cSize + 0.25 * sp.cDepth + 0.25 * sp.cLife + 0.20 * sp.cImbal;

    if (sp.score >= g_spoofThreshold)
    {
        ++p.spoofFlags;
        p.spoofScore = 0.9 * p.spoofScore + 0.1 * sp.score;
        s.spoofLog.push_back(sp);
        if (s.spoofLog.size() > 400) s.spoofLog.pop_front();
    }
}

// A flagged pull is only *confirmed* spoofing if the firm then took liquidity
// on the opposite side — i.e. the display was there to induce, not to trade.
static void ResolveSpoofFollow(Stream& s, const TradePrint& tp)
{
    const int64_t kWin = 2000ll * 1000000ll;
    for (auto it = s.spoofLog.rbegin(); it != s.spoofLog.rend(); ++it)
    {
        if (tp.ts - it->ts > kWin) break;
        if (it->confirmed) continue;
        // Bid-side display pulled, then a SELL print -> classic induce-and-sell.
        if ((it->side == 1 && tp.aggressor == 0) ||
            (it->side == 0 && tp.aggressor == 1))
        {
            it->cFollow = 1.0;
            it->score = std::min(1.0, it->score + 0.25);
            it->confirmed = true;
        }
    }
}

static void OnTrade(Stream& s, int64_t ts, double px, int32_t sz)
{
    TradePrint tp;
    tp.ts = ts; tp.px = px; tp.sz = sz;

    // Lee-Ready style classification against the prevailing quote.
    if (s.bestBid > 0.0 && s.bestAsk > 0.0)
    {
        double m = 0.5 * (s.bestBid + s.bestAsk);
        tp.aggressor = (px > m) ? 1 : (px < m ? 0 : (s.tape.empty() ? 1 : s.tape.back().aggressor));
    }
    else tp.aggressor = 1;

    if (tp.aggressor == 1) s.buyVol += sz; else s.sellVol += sz;
    ++s.tradeCount;

    s.tape.push_back(tp);
    while (s.tape.size() > 2000) s.tape.pop_front();

    ResolveSpoofFollow(s, tp);
}

// ---------------------------------------------------------------------------
//  Markout resolution: the adverse-selection engine.
//
//  Sign convention (MM's point of view):
//      bid fill  -> MM is long  -> pnl = mid_future - fill_px
//      ask fill  -> MM is short -> pnl = fill_px - mid_future
//  NEGATIVE mean markout = that MM is being run over = the flow hitting them
//  is informed. That is the tradeable read: follow the aggressor, not the MM.
//  POSITIVE mean markout = that MM is picking off the crowd -> follow the MM.
// ---------------------------------------------------------------------------
static void ResolveMarkouts(Stream& s, int64_t now)
{
    const int64_t k1s = 1000ll * 1000000ll;
    const int64_t k2s = 2000ll * 1000000ll;
    const int64_t k5s = 5000ll * 1000000ll;

    // Each queue is time-ordered and drained from the front, so every fill is
    // scored exactly once at each horizon.
    while (!s.pend1s.empty() && now - s.pend1s.front().ts >= k1s)
    {
        PendingMarkout& pm = s.pend1s.front();
        if (s.mid > 0.0 && pm.px > 0.0)
        {
            double d = (pm.side == 1) ? (s.mid - pm.px) : (pm.px - s.mid);
            s.Prof(pm.mm).markout1s.Push(d / pm.px * 1e4);   // bps
        }
        s.pend1s.pop_front();
    }

    while (!s.pend5s.empty() && now - s.pend5s.front().ts >= k5s)
    {
        PendingMarkout& pm = s.pend5s.front();
        if (s.mid > 0.0 && pm.px > 0.0)
        {
            double d = (pm.side == 1) ? (s.mid - pm.px) : (pm.px - s.mid);
            s.Prof(pm.mm).markout5s.Push(d / pm.px * 1e4);
        }
        s.pend5s.pop_front();
    }

    // Did a price improvement actually lead the market? side 1 = bullish.
    while (!s.pendInit.empty() && now - s.pendInit.front().ts >= k2s)
    {
        PendingMarkout& pi = s.pendInit.front();
        if (s.mid > 0.0 && pi.midAt > 0.0)
        {
            double r = (s.mid - pi.midAt) / pi.midAt * 1e4;
            s.Prof(pi.mm).initEdge.Push(pi.side == 1 ? r : -r);
        }
        s.pendInit.pop_front();
    }
}

// ============================================================================
//  12. LADDER APPLICATION  —  the corrected L2 state machine
// ============================================================================

static void ApplyL2(Stream& s, int position, const std::string& mmRaw,
                    int operation, int side, double price, int size)
{
    const int64_t ts = NowNs();
    s.lastUpdate = ts;

    SideLadder& L = (side == 1) ? s.bids : s.asks;
    uint16_t mmId = g_mm.Intern(mmRaw);

    // Ticks from touch BEFORE we mutate, so "depth" reflects where it landed.
    double touch = L.Touch();
    int depth = 0;
    if (touch > 0.0 && price > 0.0 && s.cfg.tick > 0.0)
        depth = (int)llround(std::fabs(touch - price) / s.cfg.tick);

    Event e;
    e.ts = ts;
    e.stream = (uint16_t)s.id;
    e.side = (uint8_t)side;
    e.depth = depth;
    e.kind = EvKind::Modify;   // neutral default: attributes no initiative



    switch (operation)
    {
    case 0:   // ---- INSERT a new row at `position`
    {
        if (position < 0 || position >= kMaxLadder) return;
        Slot v;
        v.px = price; v.sz = size; v.mm = mmId; v.birth = ts; v.touched = ts;
        L.InsertAt(position, v);

        e.kind = EvKind::Add; e.mm = mmId; e.px = price; e.sz = size; e.prevSz = 0;
        OnEvent(s, e);
        break;
    }

    case 1:   // ---- UPDATE the row at `position`
    {
        if (position < 0 || position >= L.n) return;
        Slot& sl = L.s[position];
        int32_t prev = sl.sz;

        // An empty marketMaker on an update must NOT erase the identity we
        // already hold for this row. This is half of the UNKNOWN bug.
        if (!mmRaw.empty()) sl.mm = mmId;
        else                mmId = sl.mm;

        // A price change at a fixed position is the venue re-using the row:
        // treat it as the death of the old order and the birth of a new one.
        bool repriced = (PxKey(sl.px) != PxKey(price)) && sl.px > 0.0;
        if (repriced)
        {
            Event c;
            c.ts = ts; c.stream = (uint16_t)s.id; c.side = (uint8_t)side;
            c.kind = EvKind::Cancel; c.mm = sl.mm; c.px = sl.px;
            c.sz = prev; c.prevSz = prev; c.lifeNs = ts - sl.birth; c.depth = depth;
            if (LooksLikeFill(s, ts, sl.px, prev)) c.kind = EvKind::Fill;
            OnEvent(s, c);
            sl.birth = ts;
        }

        sl.px = price;
        sl.sz = size;
        sl.touched = ts;

        if (size < prev && !repriced)
        {
            // Partial reduction: a fill or a partial cancel.
            int32_t removed = prev - size;
            e.kind = LooksLikeFill(s, ts, price, removed) ? EvKind::Fill : EvKind::Cancel;
            e.mm = mmId; e.px = price; e.sz = removed; e.prevSz = prev;
            e.lifeNs = ts - sl.birth;
        }
        else
        {
            e.kind = EvKind::Modify;
            e.mm = mmId; e.px = price; e.sz = size; e.prevSz = prev;
        }
        OnEvent(s, e);
        break;
    }

    case 2:   // ---- DELETE the row at `position`
    {
        // THE FIX. The message's marketMaker/price are unreliable here; the
        // identity of what died is the slot we are about to remove.
        Slot dead;
        if (!L.EraseAt(position, dead)) return;

        // The row is gone either way; only skip the event if it held nothing.
        if (dead.px > 0.0)
        {
            e.mm = dead.mm;
            e.px = dead.px;
            e.sz = dead.sz;
            e.prevSz = dead.sz;
            e.lifeNs = ts - dead.birth;
            e.kind = LooksLikeFill(s, ts, dead.px, dead.sz) ? EvKind::Fill : EvKind::Cancel;
            OnEvent(s, e);
        }
        break;
    }

    default: return;
    }

    // e.mm / e.kind now describe the last thing that happened to this book.
    RecomputeTouch(s, e.mm, e.kind, ts);
}

// ============================================================================
//  13. PERIODIC SAMPLING  (100 ms)  —  fair values, IC, imbalance, signal
// ============================================================================

static void SampleMMQuotes(Stream& s, int64_t now)
{
    // Reset the per-MM live view, then rebuild it from the ladder. O(rows).
    for (auto& p : s.prof)
    {
        p.fairPrev = p.fair;
        p.bestBid = 0.0; p.bestAsk = 0.0; p.bidSz = 0; p.askSz = 0;
    }

    for (int i = 0; i < s.bids.n; ++i)
    {
        Slot& sl = s.bids.s[i];
        if (sl.sz <= 0 || sl.px <= 0.0) continue;
        MMProfile& p = s.Prof(sl.mm);
        if (sl.px > p.bestBid) { p.bestBid = sl.px; p.bidSz = sl.sz; }
        else if (PxKey(sl.px) == PxKey(p.bestBid)) p.bidSz += sl.sz;
    }
    for (int i = 0; i < s.asks.n; ++i)
    {
        Slot& sl = s.asks.s[i];
        if (sl.sz <= 0 || sl.px <= 0.0) continue;
        MMProfile& p = s.Prof(sl.mm);
        if (p.bestAsk <= 0.0 || sl.px < p.bestAsk) { p.bestAsk = sl.px; p.askSz = sl.sz; }
        else if (PxKey(sl.px) == PxKey(p.bestAsk)) p.askSz += sl.sz;
    }

    for (size_t i = 0; i < s.prof.size(); ++i)
    {
        MMProfile& p = s.prof[i];
        if (!p.seen) continue;

        ++p.totalSamples;

        // Decay the manipulation score so a firm that stops is not branded
        // forever (and is not permanently excluded from the clean imbalance).
        // Half-life is roughly two minutes.
        p.spoofScore *= 0.9995;

        p.twoSided = (p.bestBid > 0.0 && p.bestAsk > 0.0);
        if (p.twoSided)
        {
            ++p.twoSidedSamples;
            p.fair = 0.5 * (p.bestBid + p.bestAsk);
        }
        else if (p.bestBid > 0.0 && s.mid > 0.0) p.fair = p.bestBid;
        else if (p.bestAsk > 0.0 && s.mid > 0.0) p.fair = p.bestAsk;

        double tot = (double)(p.bidSz + p.askSz);
        p.skew = tot > 0.0 ? (double)(p.bidSz - p.askSz) / tot : 0.0;

        // Track the CHANGE in the reservation price — the level is contaminated
        // by how wide the MM happens to be quoting; the drift is the signal.
        double d = (p.fairPrev > 0.0 && p.fair > 0.0)
                 ? (p.fair - p.fairPrev) / p.fairPrev * 1e4 : 0.0;
        p.dFair.Push((float)d);
    }
}

static void UpdateIC(Stream& s)
{
    for (auto& p : s.prof)
    {
        if (!p.seen || p.dFair.count < 200) continue;
        p.bestIC = 0.0; p.bestLag = 0;
        for (int k = 0; k < 4; ++k)
        {
            p.ic[k] = LaggedCorr(p.dFair, s.dMid, kICLags[k]);
            if (std::fabs(p.ic[k]) > std::fabs(p.bestIC))
            {
                p.bestIC = p.ic[k];
                p.bestLag = kICLags[k];
            }
        }
    }
}

static void UpdateImbalance(Stream& s)
{
    double bidTouch = 0.0, askTouch = 0.0, bidSpoof = 0.0, askSpoof = 0.0;

    for (int i = 0; i < s.bids.n; ++i)
    {
        if (PxKey(s.bids.s[i].px) != PxKey(s.bestBid)) continue;
        bidTouch += s.bids.s[i].sz;
    }
    for (int i = 0; i < s.asks.n; ++i)
    {
        if (PxKey(s.asks.s[i].px) != PxKey(s.bestAsk)) continue;
        askTouch += s.asks.s[i].sz;
    }

    // Subtract size belonging to identities we have recently flagged. A book
    // whose imbalance is manufactured should not be read as real pressure.
    for (int i = 0; i < s.bids.n; ++i)
    {
        MMProfile& p = s.Prof(s.bids.s[i].mm);
        if (p.spoofScore > g_spoofThreshold) bidSpoof += s.bids.s[i].sz;
    }
    for (int i = 0; i < s.asks.n; ++i)
    {
        MMProfile& p = s.Prof(s.asks.s[i].mm);
        if (p.spoofScore > g_spoofThreshold) askSpoof += s.asks.s[i].sz;
    }

    double t = bidTouch + askTouch;
    s.imbalance = t > 0.0 ? (bidTouch - askTouch) / t : 0.0;

    double bc = std::max(0.0, bidTouch - bidSpoof);
    double ac = std::max(0.0, askTouch - askSpoof);
    double tc = bc + ac;
    s.imbalanceClean = tc > 0.0 ? (bc - ac) / tc : 0.0;

    double cb = s.bidCxlVol.v, ca = s.askCxlVol.v;
    s.cancelAsym = (cb + ca) > 0.0 ? (ca - cb) / (cb + ca) : 0.0;   // + = bullish
}

// ---------------------------------------------------------------------------
//  Composite signal.
//
//  Each term is bounded to [-1,1] so the weights mean what they look like.
//  Fair-value drift is weighted by each MM's OWN measured IC, so participants
//  that have historically led the mid dominate and the rest contribute little.
//  This is the part that has to earn its keep in the evaluation panel.
// ---------------------------------------------------------------------------
static double CrossVenueLead(int selfId);

static void ComputeSignal(Stream& s)
{
    // --- 1. IC-weighted consensus of market-maker fair-value drift
    double num = 0.0, den = 0.0;
    for (auto& p : s.prof)
    {
        if (!p.seen || p.dFair.count < 200) continue;
        double w = std::fabs(p.bestIC);
        if (w < 0.03) continue;                 // below noise, ignore
        double sgn = p.bestIC >= 0.0 ? 1.0 : -1.0;
        num += w * sgn * p.dFair.Last();
        den += w;
    }
    double fairTerm = den > 0.0 ? Clamp(num / den / 2.0, -1.0, 1.0) : 0.0;

    // --- 2. Adverse-selection direction.
    // MMs being run over on the BID means informed sellers are hitting them.
    double advBid = 0.0, advAsk = 0.0;
    for (auto& p : s.prof)
    {
        if (!p.seen || p.markout5s.n < 30) continue;
        if (p.markout5s.mean < 0.0)
        {
            if (p.skew > 0.0) advBid += -p.markout5s.mean;
            else              advAsk += -p.markout5s.mean;
        }
    }
    double advTot = advBid + advAsk;
    double markTerm = advTot > 0.0 ? Clamp((advAsk - advBid) / advTot, -1.0, 1.0) : 0.0;

    double leadTerm = CrossVenueLead(s.id);

    s.signal =
        g_wFair    * fairTerm +
        g_wImbal   * s.imbalanceClean +
        g_wCxl     * s.cancelAsym +
        g_wMarkout * markTerm +
        g_wLead    * leadTerm;

    double wsum = g_wFair + g_wImbal + g_wCxl + g_wMarkout + g_wLead;
    if (wsum > 0.0) s.signal /= wsum;
    s.signal = Clamp(s.signal, -1.0, 1.0);
    s.sigSeries.Push((float)s.signal);
}

// Does another venue's mid lead this one? Returns that venue's latest move,
// signed, if it reliably leads. This is the cleanest structural edge here:
// the same asset cannot be in two places at once, and the slower venue must
// catch up.
static double CrossVenueLead(int selfId)
{
    double best = 0.0, bestAbs = 0.0;

    for (int i = 0; i < g_streamCount; ++i)
    {
        if (i == selfId) continue;
        double c = g_llCorr[i][selfId];          // does venue i lead us?
        if (std::fabs(c) > bestAbs && std::fabs(c) > 0.08)
        {
            bestAbs = std::fabs(c);
            best = (c >= 0.0 ? 1.0 : -1.0) *
                   Clamp(g_streams[i].dMid.Last() / 2.0, -1.0, 1.0);
        }
    }
    return best;
}

// Refreshed once per second from Tick100ms.
static void UpdateLeadLag()
{
    for (int a = 0; a < g_streamCount; ++a)
    {
        for (int b = 0; b < g_streamCount; ++b)
        {
            g_llCorr[a][b] = 0.0; g_llLag[a][b] = 0;
            if (a == b) continue;
            if (strcmp(g_streams[a].cfg.symbol, g_streams[b].cfg.symbol) != 0) continue;
            if (g_streams[a].dMid.count < 200 || g_streams[b].dMid.count < 200) continue;

            double best = 0.0; int bestLag = 0;
            for (int lag = 1; lag <= 8; ++lag)
            {
                double c = LaggedCorr(g_streams[a].dMid, g_streams[b].dMid, lag);
                if (std::fabs(c) > std::fabs(best)) { best = c; bestLag = lag; }
            }
            g_llCorr[a][b] = best;
            g_llLag[a][b] = bestLag;
        }
    }
}

static void EvaluateSignal(int64_t now)
{
    const int64_t k5 = 5000ll * 1000000ll;
    const int64_t k30 = 30000ll * 1000000ll;

    auto push = [&](std::deque<SignalEval::Pending>& q, int64_t& last, int64_t period)
    {
        if (now - last < period) return;
        last = now;
        for (int i = 0; i < g_streamCount; ++i)
        {
            Stream& s = g_streams[i];
            if (s.mid <= 0.0 || std::fabs(s.signal) < 0.05) continue;
            q.push_back({ now, i, s.signal, s.mid });
        }
    };
    push(g_eval.q5, g_eval.lastPush5, k5);
    push(g_eval.q30, g_eval.lastPush30, k30);

    auto resolve = [&](std::deque<SignalEval::Pending>& q, int64_t horizon,
                       RunStat& acc, int64_t& hits, int64_t& tot)
    {
        while (!q.empty() && now - q.front().ts >= horizon)
        {
            auto f = q.front();
            q.pop_front();
            Stream& s = g_streams[f.stream];
            if (s.mid <= 0.0 || f.mid <= 0.0) continue;

            double r = (s.mid - f.mid) / f.mid * 1e4;          // bps
            double signed_r = (f.sig >= 0.0 ? r : -r);         // + = signal was right
            acc.Push(signed_r);
            ++tot;
            if (signed_r > 0.0) ++hits;
        }
    };
    resolve(g_eval.q5, k5, g_eval.ret5s, g_eval.hits5, g_eval.tot5);
    resolve(g_eval.q30, k30, g_eval.ret30s, g_eval.hits30, g_eval.tot30);
}

static void UpdateHeat(Stream& s, int64_t now)
{
    if (s.mid <= 0.0) return;
    if (s.grid.p0 <= 0.0) { s.grid.tick = s.cfg.tick; s.grid.Recenter(s.mid); }
    if (now - s.grid.lastCol < kGridDtNs) return;
    s.grid.lastCol = now;

    // Re-centre if the market has walked out of the raster.
    int rmid = s.grid.RowOf(s.mid);
    if (rmid < 16 || rmid > kGridRows - 16) { s.grid.Recenter(s.mid); s.grid.count = 0; }

    int c = s.grid.head;
    memset(s.grid.bid[c], 0, sizeof(s.grid.bid[c]));
    memset(s.grid.ask[c], 0, sizeof(s.grid.ask[c]));

    uint16_t peak = 0;
    for (int i = 0; i < s.bids.n; ++i)
    {
        int r = s.grid.RowOf(s.bids.s[i].px);
        if (r < 0) continue;
        s.grid.bid[c][r] = (uint16_t)std::min(65535, (int)s.grid.bid[c][r] + s.bids.s[i].sz);
        peak = std::max(peak, s.grid.bid[c][r]);
    }
    for (int i = 0; i < s.asks.n; ++i)
    {
        int r = s.grid.RowOf(s.asks.s[i].px);
        if (r < 0) continue;
        s.grid.ask[c][r] = (uint16_t)std::min(65535, (int)s.grid.ask[c][r] + s.asks.s[i].sz);
        peak = std::max(peak, s.grid.ask[c][r]);
    }
    s.grid.colMax[c] = peak;
    s.grid.mid[c] = (float)s.mid;
    s.grid.head = (s.grid.head + 1) % kGridCols;
    if (s.grid.count < kGridCols) ++s.grid.count;
}

static void Tick100ms(int64_t now)
{
    for (int i = 0; i < g_streamCount; ++i)
    {
        Stream& s = g_streams[i];

        double d = (s.prevMid > 0.0 && s.mid > 0.0)
                 ? (s.mid - s.prevMid) / s.prevMid * 1e4 : 0.0;
        s.dMid.Push((float)d);
        s.midSeries.Push((float)s.mid);
        s.prevMid = s.mid;

        SampleMMQuotes(s, now);
        UpdateImbalance(s);
        ResolveMarkouts(s, now);
        UpdateHeat(s, now);

        if (now - s.rateWindowStart > 1000000000ll)
        {
            s.evRate.Push((double)s.rateWindowCount);
            s.rateWindowCount = 0;
            s.rateWindowStart = now;
        }
    }

    // The correlation work is O(window) per pair, so it runs at 1 Hz, not 10.
    static int64_t lastIC = 0;
    if (now - lastIC > 1000000000ll)
    {
        lastIC = now;
        for (int i = 0; i < g_streamCount; ++i) UpdateIC(g_streams[i]);
        UpdateLeadLag();
    }

    for (int i = 0; i < g_streamCount; ++i) ComputeSignal(g_streams[i]);
    EvaluateSignal(now);
}

// ============================================================================
//  14. FINGERPRINTING  —  behavioural similarity between identities
// ============================================================================
//
//  HONEST FRAMING: this does NOT de-anonymise anybody. You cannot recover a
//  firm's identity from L2 alone, and any tool claiming otherwise is lying.
//  What this does is measure whether the ANONYMOUS residual behaves like a
//  named MPID's *algorithm* — same lifetime distribution, same size
//  quantisation, same replenish latency, same standoff from the touch.
//  A high score is a hypothesis worth testing, not an identification.
// ============================================================================

static const int kFPDims = 8;
static const char* kFPNames[kFPDims] =
{
    "sub-100ms frac", "log median life", "cancel/add", "round-lot frac",
    "mean depth (ticks)", "two-sided frac", "refill rate", "log mean gap"
};

static void Fingerprint(const MMProfile& p, double f[kFPDims])
{
    f[0] = p.SubSecondFrac();
    f[1] = std::log10(std::max(1.0, p.MedianLifeMs())) / 5.0;
    f[2] = Clamp(p.CancelRatio(), 0.0, 2.0) / 2.0;
    f[3] = (p.roundLots + p.oddLots) > 0
         ? (double)p.roundLots / (double)(p.roundLots + p.oddLots) : 0.0;
    f[4] = Clamp(p.depthTicks.mean / 20.0, 0.0, 1.0);
    f[5] = p.TwoSidedFrac();
    f[6] = p.adds > 0 ? Clamp((double)p.refills / (double)p.adds, 0.0, 1.0) : 0.0;
    f[7] = p.gapMs.n > 10 ? Clamp(std::log10(std::max(1.0, p.gapMs.mean)) / 5.0, 0.0, 1.0) : 0.0;
}

static double FPSimilarity(const MMProfile& a, const MMProfile& b)
{
    double fa[kFPDims], fb[kFPDims];
    Fingerprint(a, fa);
    Fingerprint(b, fb);
    double d2 = 0.0;
    for (int i = 0; i < kFPDims; ++i) { double d = fa[i] - fb[i]; d2 += d * d; }
    return 1.0 - Clamp(std::sqrt(d2 / kFPDims), 0.0, 1.0);
}

// ============================================================================
//  15. SIMULATED FEED  (market-closed testing)
// ============================================================================
//
//  Four synthetic participants with deliberately different personalities, so
//  the detectors can be validated when the tape is shut:
//      SIMA — patient, wide, two-sided, long-lived        (real market maker)
//      SIMB — sub-50ms flicker at the touch               (HFT quoter)
//      SIMC — refills the same price after depletion      (iceberg)
//      SIMD — large size 3-6 ticks out, pulled fast, then trades the other way
//             (this one SHOULD light up the spoof panel; if it does not, the
//              detector is broken)
// ============================================================================

struct SimState
{
    double  px = 227.50;
    std::mt19937 rng{ 12345 };
    int64_t next = 0;
    int     step = 0;
};
static SimState g_sim[kMaxStreams];

static void SimTick(Stream& s, SimState& st, int64_t now)
{
    if (now < st.next) return;
    st.next = now + 8ll * 1000000ll;   // ~120 msgs/sec/stream
    ++st.step;

    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::normal_distribution<double> N(0.0, 1.0);

    st.px += N(st.rng) * 0.004;
    double tick = s.cfg.tick;
    double mid = std::round(st.px / tick) * tick;

    // Insert at the price-sorted position, the way a real venue does, so that
    // row 0 really is the touch and the derived mid is meaningful.
    auto post = [&](const char* mm, int side, int ticksOut, int size)
    {
        double px = side == 1 ? mid - (ticksOut + 1) * tick : mid + (ticksOut + 1) * tick;
        SideLadder& L = (side == 1) ? s.bids : s.asks;
        int pos = L.n;
        for (int i = 0; i < L.n; ++i)
        {
            bool before = (side == 1) ? (px > L.s[i].px) : (px < L.s[i].px);
            if (before) { pos = i; break; }
        }
        if (pos >= kMaxLadder) return;
        ApplyL2(s, pos, mm, 0, side, px, size);
    };
    auto pull = [&](int side, int pos)
    {
        SideLadder& L = (side == 1) ? s.bids : s.asks;
        if (L.n > pos) ApplyL2(s, pos, "", 2, side, 0.0, 0);
    };

    int side = U(st.rng) < 0.5 ? 1 : 0;

    // SIMA: slow, wide, two-sided.
    if (st.step % 20 == 0) post("SIMA", side, 2 + (int)(U(st.rng) * 3), 200 + (int)(U(st.rng) * 400));
    if (st.step % 23 == 0) pull(side, 3);

    // SIMB: flicker at the touch.
    if (st.step % 2 == 0) post("SIMB", side, 0, 100);
    if (st.step % 2 == 1) pull(side, 0);

    // SIMC: iceberg refill at one price.
    if (st.step % 11 == 0) post("SIMC", 1, 1, 500);
    if (st.step % 11 == 5) pull(1, 1);

    // SIMD: layer then flip.
    if (st.step % 40 == 0) post("SIMD", 1, 4, 3000);
    if (st.step % 40 == 6)
    {
        pull(1, 4);
        OnTrade(s, now, s.bestBid > 0 ? s.bestBid : mid, 800);   // sells into it
    }

    // Anonymous background flow.
    if (st.step % 3 == 0) post("", side, (int)(U(st.rng) * 6), 100 + (int)(U(st.rng) * 900));
    if (st.step % 4 == 0) pull(side, (int)(U(st.rng) * std::max(1, (side ? s.bids.n : s.asks.n))));

    if (st.step % 7 == 0) OnTrade(s, now, mid, 100 + (int)(U(st.rng) * 300));
}

// ============================================================================
//  16. IB INGEST
// ============================================================================

static std::vector<std::string> g_depthExchanges;
static std::deque<std::string>  g_log;

static void LogLine(const std::string& s)
{
    g_log.push_back(s);
    if (g_log.size() > 400) g_log.pop_front();
}

static int StreamForDepthReq(int reqId)
{
    int idx = reqId - 1000;
    return (idx >= 0 && idx < g_streamCount) ? idx : -1;
}
// The trade tape is requested once per SYMBOL, not once per stream: the prints
// are identical across venues and tick-by-tick lines are as scarce as depth
// lines. reqId 2000+k identifies the k-th distinct symbol; a print is fanned
// out to every stream carrying that symbol.
static std::vector<std::string> g_tapeSymbols;

static int TapeSymbolForReq(int reqId)
{
    int idx = reqId - 2000;
    return (idx >= 0 && idx < (int)g_tapeSymbols.size()) ? idx : -1;
}

// Derives from the existing wrapper so connection/contract handling is reused;
// only the data paths we actually care about are overridden.
class AlphaWrapper : public IbWrapperBase
{
public:
    void updateMktDepthL2(TickerId id, int position, const std::string& marketMaker,
                          int operation, int side, double price, int size) override
    {
        int si = StreamForDepthReq((int)id);
        if (si < 0) return;
        g_streams[si].live = true;
        ApplyL2(g_streams[si], position, marketMaker, operation, side, price, size);
    }

    // Venues without attributed depth arrive here (no MPID at all).
    void updateMktDepth(TickerId id, int position, int operation, int side,
                        double price, int size)
    {
        int si = StreamForDepthReq((int)id);
        if (si < 0) return;
        g_streams[si].live = true;
        ApplyL2(g_streams[si], position, "", operation, side, price, size);
    }

    void tickByTickAllLast(int reqId, int /*tickType*/, time_t /*time*/, double price,
                           int size, const TickAttrib& /*a*/, const std::string& /*exch*/,
                           const std::string& /*cond*/)
    {
        int k = TapeSymbolForReq(reqId);
        if (k < 0) return;
        int64_t now = NowNs();
        for (int i = 0; i < g_streamCount; ++i)
            if (g_tapeSymbols[k] == g_streams[i].cfg.symbol)
                OnTrade(g_streams[i], now, price, size);
    }

    void mktDepthExchanges(const std::vector<DepthMktDataDescription>& d)
    {
        g_depthExchanges.clear();
        for (auto& x : d)
            g_depthExchanges.push_back(x.exchange + " (" + x.secType + "/" + x.serviceDataType + ")");
        LogLine("Depth exchanges available: " + std::to_string(d.size()));
    }

    void error(int id, int code, const std::string& msg)
    {
        if (code == 2104 || code == 2106 || code == 2119 || code == 2158) return;

        char buf[512];
        snprintf(buf, sizeof(buf), "[%d] req=%d %s", code, id, msg.c_str());
        LogLine(buf);

        int si = StreamForDepthReq(id);
        if (si >= 0)
        {
            g_streams[si].lastError = buf;
            // 309 = too many depth lines, 354 = not subscribed, 2152 = no L2 perm
            if (code == 309 || code == 354 || code == 2152 || code == 10197)
                g_streams[si].live = false;
        }
    }

    void error(int id, int code, const std::string& msg, const std::string& adv)
    {
        error(id, code, msg);
        if (!adv.empty()) LogLine("  advanced: " + adv);
    }
};

// ============================================================================
//  17. UI HELPERS
// ============================================================================

static ImVec4 SignalColor(double v)
{
    if (v > 0.0) return ImVec4(0.15f, 0.85f, 0.45f, 1.0f);
    if (v < 0.0) return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
}

// Compact inline chart. Draws a zero line when the series is signed.
template <int N>
static void Spark(const char* id, const Ring<N>& r, ImVec2 size, ImU32 col, bool signedSeries)
{
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(12, 12, 16, 255));

    if (r.count < 2) { ImGui::Dummy(size); return; }

    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < r.count; ++i) { float v = r.At(i); lo = std::min(lo, v); hi = std::max(hi, v); }
    if (signedSeries) { float m = std::max(std::fabs(lo), std::fabs(hi)); lo = -m; hi = m; }
    if (hi - lo < 1e-9f) { hi = lo + 1e-9f; }

    auto Y = [&](float v) { return p0.y + size.y - (v - lo) / (hi - lo) * size.y; };

    if (signedSeries)
        dl->AddLine(ImVec2(p0.x, Y(0.0f)), ImVec2(p0.x + size.x, Y(0.0f)), IM_COL32(70, 70, 90, 255), 1.0f);

    int step = std::max(1, r.count / (int)size.x);
    ImVec2 prev;
    bool first = true;
    for (int i = 0; i < r.count; i += step)
    {
        float x = p0.x + (float)i / (float)r.count * size.x;
        ImVec2 pt(x, Y(r.At(i)));
        if (!first) dl->AddLine(prev, pt, col, 1.3f);
        prev = pt; first = false;
    }
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(60, 60, 75, 255));
    ImGui::Dummy(size);
    (void)id;
}

// ============================================================================
//  18. PANELS
// ============================================================================

static void PanelStreams(EClientSocket* client)
{
    ImGui::Begin("1 - Streams & Feed Health");

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "SOURCE");
    ImGui::SameLine();
    if (g_simMode) ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "SIMULATION (synthetic)");
    else           ImGui::TextColored(ImVec4(0.2f, 1, 0.4f, 1), "IBKR LIVE");
    ImGui::SameLine();
    if (ImGui::SmallButton(g_simMode ? "switch to LIVE" : "switch to SIM")) g_simMode = !g_simMode;

    ImGui::SameLine();
    if (g_recording)
    {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "REC %lld rows", (long long)g_recLines);
        ImGui::SameLine();
        if (ImGui::SmallButton("stop")) RecClose();
    }
    else if (ImGui::SmallButton("record CSV")) RecOpen();

    ImGui::Separator();
    ImGui::TextWrapped(
        "IBKR allows only a few SIMULTANEOUS depth subscriptions (commonly 3). "
        "Error 309 below means you asked for more than your account allows; "
        "354 / 2152 mean you lack the depth entitlement for that venue.");
    ImGui::Separator();

    if (ImGui::BeginTable("streams", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Venue", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Bid", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Ask", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("ev/s", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("MMs", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < g_streamCount; ++i)
        {
            Stream& s = g_streams[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(i).c_str(), g_selStream == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
                g_selStream = i;

            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", s.cfg.symbol);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%s", s.cfg.venue);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(ImVec4(0, 0.9f, 0.9f, 1), "%.2f x%d", s.bestBid, s.bids.n ? s.bids.s[0].sz : 0);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "%.2f x%d", s.bestAsk, s.asks.n ? s.asks.s[0].sz : 0);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%.0f", s.evRate.v);
            ImGui::TableSetColumnIndex(6);
            {
                int active = 0;
                for (auto& p : s.prof) if (p.seen) ++active;
                ImGui::Text("%d", active);
            }
            ImGui::TableSetColumnIndex(7);
            if (!s.lastError.empty())
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", s.lastError.c_str());
            else if (s.live)
                ImGui::TextColored(ImVec4(0.2f, 1, 0.4f, 1), "streaming  (%lld ev, %lld prints)",
                    (long long)s.evCount, (long long)s.tradeCount);
            else
                ImGui::TextDisabled("waiting...");
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("Query venues with depth (reqMktDepthExchanges)") && client && client->isConnected())
        client->reqMktDepthExchanges();

    if (!g_depthExchanges.empty() && ImGui::TreeNode("Depth venues your account can see"))
    {
        for (auto& e : g_depthExchanges) ImGui::BulletText("%s", e.c_str());
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("API log"))
    {
        ImGui::BeginChild("log", ImVec2(0, 140), true);
        for (auto& l : g_log) ImGui::TextUnformatted(l.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }
    ImGui::End();
}

static void PanelBookmap()
{
    Stream& s = g_streams[g_selStream];
    char title[128];
    snprintf(title, sizeof(title), "2 - Bookmap  [%s @ %s]###bookmap", s.cfg.symbol, s.cfg.venue);
    ImGui::Begin(title);

    ImGui::Checkbox("heat", &g_showHeatmap); ImGui::SameLine();
    ImGui::Checkbox("MM levels", &g_showMMLines); ImGui::SameLine();
    ImGui::Checkbox("mid", &g_showMid); ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("gain", &g_heatGain, 0.2f, 4.0f, "%.1f");

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float labelW = 66.0f;
    float w = std::max(200.0f, avail.x - labelW);
    float h = std::max(160.0f, avail.y - 8.0f);
    ImVec2 p1(p0.x + w, p0.y + h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(6, 6, 9, 255));

    HeatGrid& g = s.grid;
    if (g.count < 2 || g.p0 <= 0.0) { ImGui::TextDisabled("collecting..."); ImGui::End(); return; }

    // Never draw more columns than the panel has pixels. Without this, 2880
    // columns x 192 rows is ~550k draw calls per frame and the UI dies.
    int   stride = std::max(1, (int)std::ceil((float)g.count / std::max(1.0f, w)));
    int   ncols = std::max(1, g.count / stride);
    float colW = w / (float)ncols;
    float rowH = h / (float)kGridRows;

    auto ColAt = [&](int k) { return (g.head - g.count + k * stride + kGridCols * 2) % kGridCols; };

    // Auto-scale intensity from the cached per-column peaks.
    int maxSz = 1;
    for (int k = 0; k < ncols; ++k) maxSz = std::max(maxSz, (int)g.colMax[ColAt(k)]);

    if (g_showHeatmap)
    {
        for (int k = 0; k < ncols; ++k)
        {
            int c = ColAt(k);
            if (!g.colMax[c]) continue;              // empty column, skip 192 tests
            float x = p0.x + k * colW;
            for (int r = 0; r < kGridRows; ++r)
            {
                float y = p1.y - (r + 1) * rowH;
                if (g.bid[c][r])
                {
                    float t = (float)Clamp((double)g.bid[c][r] / maxSz * g_heatGain, 0.0, 1.0);
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW + 0.6f, y + rowH),
                        IM_COL32(20, 200, 90, (int)(30 + 210 * t)));
                }
                if (g.ask[c][r])
                {
                    float t = (float)Clamp((double)g.ask[c][r] / maxSz * g_heatGain, 0.0, 1.0);
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW + 0.6f, y + rowH),
                        IM_COL32(225, 55, 55, (int)(30 + 210 * t)));
                }
            }
        }
    }

    if (g_showMid)
    {
        ImVec2 prev; bool first = true;
        for (int k = 0; k < ncols; ++k)
        {
            int c = ColAt(k);
            if (g.mid[c] <= 0.0f) continue;
            int r = g.RowOf(g.mid[c]);
            if (r < 0) continue;
            ImVec2 pt(p0.x + k * colW, p1.y - (r + 0.5f) * rowH);
            if (!first) dl->AddLine(prev, pt, IM_COL32(255, 255, 255, 210), 1.6f);
            prev = pt; first = false;
        }
    }

    // Named market-maker levels, drawn from the CORRECTED ladder.
    if (g_showMMLines)
    {
        auto drawSide = [&](SideLadder& L, int side)
        {
            for (int i = 0; i < L.n; ++i)
            {
                Slot& sl = L.s[i];
                if (sl.sz < (int)g_minMMSize || sl.px <= 0.0) continue;
                if (g_hideAnon && !IsAttributed(sl.mm)) continue;
                int r = g.RowOf(sl.px);
                if (r < 0) continue;

                MMProfile& p = s.Prof(sl.mm);
                float y = p1.y - (r + 0.5f) * rowH;
                bool attributed = IsAttributed(sl.mm);
                ImU32 col = side == 1 ? IM_COL32(0, 255, 235, 235) : IM_COL32(255, 165, 20, 235);
                if (!attributed) col = IM_COL32(150, 150, 175, 160);

                dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), col, attributed ? 1.8f : 1.0f);

                char buf[96];
                const char* tag = "";
                if (p.spoofScore > g_spoofThreshold) tag = " SPOOF?";
                else if (p.refills > 3)              tag = " ICE";
                snprintf(buf, sizeof(buf), "%s %d%s", g_mm.Name(sl.mm), sl.sz, tag);
                dl->AddText(ImVec2(p1.x + 5, y - 7), IM_COL32(0, 0, 0, 255), buf);
                dl->AddText(ImVec2(p1.x + 4, y - 8), col, buf);
            }
        };
        drawSide(s.bids, 1);
        drawSide(s.asks, 0);
    }

    dl->AddRect(p0, p1, IM_COL32(70, 70, 85, 255));
    ImGui::Dummy(ImVec2(w, h));
    ImGui::End();
}

static void PanelScorecard()
{
    Stream& s = g_streams[g_selStream];
    ImGui::Begin("3 - Market Maker Scorecard");

    ImGui::TextWrapped(
        "One row per identity on this venue. The two columns that decide whether an "
        "MM is worth watching are IC (does its fair value LEAD the mid?) and MARKOUT "
        "(is it being run over?). Negative markout with a large sample means the flow "
        "hitting that MM is informed - follow the aggressor. Positive means the MM is "
        "picking off the crowd - follow the MM.");
    ImGui::Separator();
    ImGui::Checkbox("hide unattributed", &g_hideAnon);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("min size", &g_minMMSize, 0.0f, 500.0f, "%.0f");

    const int kCols = 15;
    if (ImGui::BeginTable("mm", kCols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("MM");
        ImGui::TableSetupColumn("Fair");
        ImGui::TableSetupColumn("vs Mid(bp)");
        ImGui::TableSetupColumn("Skew");
        ImGui::TableSetupColumn("IC");
        ImGui::TableSetupColumn("Lag");
        ImGui::TableSetupColumn("Mkout5s(bp)");
        ImGui::TableSetupColumn("t");
        ImGui::TableSetupColumn("Init");
        ImGui::TableSetupColumn("Adds");
        ImGui::TableSetupColumn("Cxl/Add");
        ImGui::TableSetupColumn("Fill%");
        ImGui::TableSetupColumn("MedLife");
        ImGui::TableSetupColumn("Refill");
        ImGui::TableSetupColumn("Spoof");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < s.prof.size(); ++i)
        {
            MMProfile& p = s.prof[i];
            if (!p.seen) continue;
            if (g_hideAnon && !IsAttributed((uint16_t)i)) continue;

            ImGui::TableNextRow();
            int c = 0;

            ImGui::TableSetColumnIndex(c++);
            if (IsAttributed((uint16_t)i))
                ImGui::TextColored(ImVec4(1, 1, 0.55f, 1), "%s", g_mm.Name((uint16_t)i));
            else
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.8f, 1), "%s", g_mm.Name((uint16_t)i));

            ImGui::TableSetColumnIndex(c++);
            if (p.fair > 0.0) ImGui::Text("%.3f", p.fair); else ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(c++);
            if (p.fair > 0.0 && s.mid > 0.0)
            {
                double bp = (p.fair - s.mid) / s.mid * 1e4;
                ImGui::TextColored(SignalColor(bp), "%+.1f", bp);
            }
            else ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(c++);
            ImGui::TextColored(SignalColor(p.skew), "%+.2f", p.skew);

            ImGui::TableSetColumnIndex(c++);
            if (p.dFair.count > 200)
                ImGui::TextColored(std::fabs(p.bestIC) > 0.08
                    ? ImVec4(0.3f, 1, 0.5f, 1) : ImVec4(0.55f, 0.55f, 0.55f, 1), "%+.3f", p.bestIC);
            else ImGui::TextDisabled("...");

            ImGui::TableSetColumnIndex(c++);
            if (p.bestLag) ImGui::Text("%.1fs", p.bestLag * 0.1); else ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(c++);
            if (p.markout5s.n >= 20)
                ImGui::TextColored(SignalColor(p.markout5s.mean), "%+.2f", p.markout5s.mean);
            else ImGui::TextDisabled("n=%lld", (long long)p.markout5s.n);

            ImGui::TableSetColumnIndex(c++);
            if (p.markout5s.n >= 20)
            {
                double t = p.markout5s.T();
                ImGui::TextColored(std::fabs(t) > 2.0
                    ? ImVec4(1, 1, 0.4f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1), "%.1f", t);
            }
            else ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(c++); ImGui::Text("%d", p.initiations);
            ImGui::TableSetColumnIndex(c++); ImGui::Text("%lld", (long long)p.adds);

            ImGui::TableSetColumnIndex(c++);
            double cr = p.CancelRatio();
            ImGui::TextColored(cr > 0.95 ? ImVec4(1, 0.6f, 0.3f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1), "%.2f", cr);

            ImGui::TableSetColumnIndex(c++);
            ImGui::Text("%.0f%%", p.FillRatio() * 100.0);

            ImGui::TableSetColumnIndex(c++);
            ImGui::Text("%.0fms", p.MedianLifeMs());

            ImGui::TableSetColumnIndex(c++);
            if (p.refills > 3) ImGui::TextColored(ImVec4(1, 1, 0.3f, 1), "%d/%.0fms",
                p.refills, p.refillLatMs.mean);
            else ImGui::Text("%d", p.refills);

            ImGui::TableSetColumnIndex(c++);
            if (p.spoofScore > g_spoofThreshold)
                ImGui::TextColored(ImVec4(1, 0.25f, 0.25f, 1), "%.2f (%d)", p.spoofScore, p.spoofFlags);
            else ImGui::TextDisabled("%.2f", p.spoofScore);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void PanelManipulation()
{
    Stream& s = g_streams[g_selStream];
    ImGui::Begin("4 - Cancellation, Flicker & Layering");

    ImGui::TextWrapped(
        "Large-and-cancelled is NOT spoofing - that is ordinary market making. The flag "
        "here is conjunctive: unusually large FOR THAT FIRM, posted away from the touch, "
        "pulled quickly, skewing the visible book, and then followed by aggression on the "
        "OPPOSITE side. Every component is shown so you can argue with any flag. Only rows "
        "marked CONFIRMED had the follow-through.");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("flag threshold", &g_spoofThreshold, 0.2f, 1.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("flicker cutoff (ms)", &g_flickerMs, 10.0f, 1000.0f, "%.0f");
    ImGui::Separator();

    ImGui::Text("Cancellation asymmetry: ");
    ImGui::SameLine();
    ImGui::TextColored(SignalColor(s.cancelAsym), "%+.2f", s.cancelAsym);
    ImGui::SameLine();
    ImGui::TextDisabled("(+ = asks being pulled faster = bullish)");

    // Flicker: add-then-cancel inside the cutoff. Sustained high rates are the
    // clearest evidence you are looking at a machine rather than a desk, and
    // the rate itself is a stable per-firm fingerprint.
    if (ImGui::CollapsingHeader("Flicker / ping rate by participant", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("flick", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("MM");
            ImGui::TableSetupColumn("Flickers");
            ImGui::TableSetupColumn("% of quotes");
            ImGui::TableSetupColumn("Median life");
            ImGui::TableSetupColumn("Read");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < s.prof.size(); ++i)
            {
                MMProfile& p = s.prof[i];
                if (!p.seen || p.flickers == 0) continue;
                double frac = p.SubSecondFrac();
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", g_mm.Name((uint16_t)i));
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", p.flickers);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(frac > 0.5 ? ImVec4(1, 0.6f, 0.3f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1),
                    "%.0f%%", frac * 100.0);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.0fms", p.MedianLifeMs());
                ImGui::TableSetColumnIndex(4);
                if (frac > 0.7)      ImGui::TextDisabled("latency-sensitive machine");
                else if (frac > 0.3) ImGui::TextDisabled("mixed / reactive quoting");
                else                 ImGui::TextDisabled("patient");
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::BeginTable("spoof", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Age");
        ImGui::TableSetupColumn("MM");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Life");
        ImGui::TableSetupColumn("size");
        ImGui::TableSetupColumn("depth");
        ImGui::TableSetupColumn("life");
        ImGui::TableSetupColumn("imbal");
        ImGui::TableSetupColumn("Score");
        ImGui::TableHeadersRow();

        int64_t now = NowNs();
        for (auto it = s.spoofLog.rbegin(); it != s.spoofLog.rend(); ++it)
        {
            ImGui::TableNextRow();
            int c = 0;
            ImGui::TableSetColumnIndex(c++); ImGui::Text("%.1fs", (now - it->ts) / 1e9);
            ImGui::TableSetColumnIndex(c++); ImGui::Text("%s", g_mm.Name(it->mm));
            ImGui::TableSetColumnIndex(c++);
            if (it->side == 1) ImGui::TextColored(ImVec4(0, 1, 1, 1), "BID");
            else               ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "ASK");
            ImGui::TableSetColumnIndex(c++); ImGui::Text("%.2f", it->px);
            ImGui::TableSetColumnIndex(c++); ImGui::Text("%d", it->sz);
            ImGui::TableSetColumnIndex(c++); ImGui::Text("%.0fms", it->lifeMs);
            ImGui::TableSetColumnIndex(c++); ImGui::TextDisabled("%.2f", it->cSize);
            ImGui::TableSetColumnIndex(c++); ImGui::TextDisabled("%.2f", it->cDepth);
            ImGui::TableSetColumnIndex(c++); ImGui::TextDisabled("%.2f", it->cLife);
            ImGui::TableSetColumnIndex(c++); ImGui::TextDisabled("%.2f", it->cImbal);
            ImGui::TableSetColumnIndex(c++);
            if (it->confirmed)
                ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "%.2f CONFIRMED", it->score);
            else
                ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%.2f", it->score);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void PanelAnonymous()
{
    Stream& s = g_streams[g_selStream];
    ImGui::Begin("5 - Anonymous / NSDQ Analysis");

    ImGui::TextWrapped(
        "WHAT NSDQ ACTUALLY IS: on NASDAQ TotalView, 'NSDQ' is not a firm. It is the "
        "aggregate of every order whose owner chose NOT to attribute. Every large firm "
        "sits inside it. You cannot recover WHO from L2 - anyone claiming otherwise is "
        "selling something.\n\n"
        "WHAT IS ACTUALLY RECOVERABLE: (a) how anonymous size behaves relative to named "
        "size at the same price, and (b) whether the anonymous residual's ALGORITHM "
        "fingerprint matches a named MPID's. A high similarity below is a hypothesis to "
        "test, never an identification.");
    ImGui::Separator();

    // Attributed vs anonymous share of resting size.
    double namedBid = 0, anonBid = 0, namedAsk = 0, anonAsk = 0;
    for (int i = 0; i < s.bids.n; ++i)
        (IsAttributed(s.bids.s[i].mm) ? namedBid : anonBid) += s.bids.s[i].sz;
    for (int i = 0; i < s.asks.n; ++i)
        (IsAttributed(s.asks.s[i].mm) ? namedAsk : anonAsk) += s.asks.s[i].sz;

    double totB = namedBid + anonBid, totA = namedAsk + anonAsk;
    ImGui::Text("Displayed size, bid:  named %.0f (%.0f%%)   anonymous %.0f (%.0f%%)",
        namedBid, totB > 0 ? namedBid / totB * 100 : 0, anonBid, totB > 0 ? anonBid / totB * 100 : 0);
    ImGui::Text("Displayed size, ask:  named %.0f (%.0f%%)   anonymous %.0f (%.0f%%)",
        namedAsk, totA > 0 ? namedAsk / totA * 100 : 0, anonAsk, totA > 0 ? anonAsk / totA * 100 : 0);

    double anonImb = (anonBid + anonAsk) > 0 ? (anonBid - anonAsk) / (anonBid + anonAsk) : 0;
    double namedImb = (namedBid + namedAsk) > 0 ? (namedBid - namedAsk) / (namedBid + namedAsk) : 0;
    ImGui::Text("Anonymous imbalance %+.2f   vs   Named imbalance %+.2f", anonImb, namedImb);
    if (std::fabs(anonImb - namedImb) > 0.35)
        ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1),
            "DIVERGENCE: the named book and the anonymous book disagree. The anonymous "
            "side is usually the one with the position.");

    ImGui::Separator();
    ImGui::Text("Algorithm fingerprint similarity to the anonymous residual");

    // Find the dominant unattributed identity on this stream.
    int anonIdx = -1; int64_t bestAdds = 0;
    for (size_t i = 0; i < s.prof.size(); ++i)
        if (s.prof[i].seen && !IsAttributed((uint16_t)i) && s.prof[i].adds > bestAdds)
        { bestAdds = s.prof[i].adds; anonIdx = (int)i; }

    if (anonIdx < 0) { ImGui::TextDisabled("no unattributed flow yet"); ImGui::End(); return; }
    ImGui::Text("Reference: %s  (%lld adds)", g_mm.Name((uint16_t)anonIdx), (long long)bestAdds);

    struct Row { int idx; double sim; };
    std::vector<Row> rows;
    for (size_t i = 0; i < s.prof.size(); ++i)
    {
        if (!s.prof[i].seen || (int)i == anonIdx) continue;
        if (!IsAttributed((uint16_t)i)) continue;
        if (s.prof[i].adds < 50) continue;
        rows.push_back({ (int)i, FPSimilarity(s.prof[anonIdx], s.prof[i]) });
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sim > b.sim; });

    if (ImGui::BeginTable("fp", 2 + kFPDims, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("MM");
        ImGui::TableSetupColumn("Similarity");
        for (int i = 0; i < kFPDims; ++i) ImGui::TableSetupColumn(kFPNames[i]);
        ImGui::TableHeadersRow();

        double fr[kFPDims];
        Fingerprint(s.prof[anonIdx], fr);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1), "%s", g_mm.Name((uint16_t)anonIdx));
        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("reference");
        for (int i = 0; i < kFPDims; ++i)
        { ImGui::TableSetColumnIndex(2 + i); ImGui::TextDisabled("%.2f", fr[i]); }

        for (auto& r : rows)
        {
            double f[kFPDims];
            Fingerprint(s.prof[r.idx], f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(1, 1, 0.55f, 1), "%s", g_mm.Name((uint16_t)r.idx));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(r.sim > 0.85 ? ImVec4(1, 1, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                "%.3f", r.sim);
            for (int i = 0; i < kFPDims; ++i)
            { ImGui::TableSetColumnIndex(2 + i); ImGui::Text("%.2f", f[i]); }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void PanelLeadLag()
{
    ImGui::Begin("6 - Competition & Cross-Venue Lead-Lag");

    ImGui::TextWrapped(
        "Same asset, different venues. The asset cannot be in two places at once, so "
        "whichever venue moves first, the others must follow. Read: row LEADS column by "
        "the lag shown, with that correlation. Only |r| > 0.08 is worth anything.");
    ImGui::Separator();

    if (ImGui::BeginTable("ll", g_streamCount + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("leader \\ follower");
        for (int i = 0; i < g_streamCount; ++i)
            ImGui::TableSetupColumn(g_streams[i].cfg.venue);
        ImGui::TableHeadersRow();

        for (int a = 0; a < g_streamCount; ++a)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s %s", g_streams[a].cfg.symbol, g_streams[a].cfg.venue);
            for (int b = 0; b < g_streamCount; ++b)
            {
                ImGui::TableSetColumnIndex(b + 1);
                if (a == b) { ImGui::TextDisabled("-"); continue; }
                if (strcmp(g_streams[a].cfg.symbol, g_streams[b].cfg.symbol) != 0)
                { ImGui::TextDisabled("n/a"); continue; }

                double best = g_llCorr[a][b];
                int    bestLag = g_llLag[a][b];
                if (std::fabs(best) > 0.08)
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "%+.2f @%.1fs", best, bestLag * 0.1);
                else
                    ImGui::TextDisabled("%+.2f", best);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Quote initiative on %s @ %s  (who moves the BBO first)",
        g_streams[g_selStream].cfg.symbol, g_streams[g_selStream].cfg.venue);

    Stream& s = g_streams[g_selStream];
    if (ImGui::BeginTable("init", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("MM");
        ImGui::TableSetupColumn("Initiations");
        ImGui::TableSetupColumn("Joins");
        ImGui::TableSetupColumn("Fades");
        ImGui::TableSetupColumn("Edge after init (bp)");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.prof.size(); ++i)
        {
            MMProfile& p = s.prof[i];
            if (!p.seen || (p.initiations + p.joins + p.fades) == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", g_mm.Name((uint16_t)i));
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", p.initiations);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", p.joins);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", p.fades);
            ImGui::TableSetColumnIndex(4);
            if (p.initEdge.n > 10)
                ImGui::TextColored(SignalColor(p.initEdge.mean), "%+.2f", p.initEdge.mean);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void PanelSignal()
{
    ImGui::Begin("7 - Signal & Live Evaluation");

    ImGui::TextWrapped(
        "This is the only panel that can tell you whether any of the rest is real. The "
        "signal is logged against the REALISED forward return as it happens. If the "
        "t-stat does not clear about 2 with a few hundred observations, nothing here is "
        "tradeable yet - and you should not size anything on it.");
    ImGui::Separator();

    for (int i = 0; i < g_streamCount; ++i)
    {
        Stream& s = g_streams[i];
        ImGui::PushID(i);
        ImGui::Text("%-6s %-8s", s.cfg.symbol, s.cfg.venue);
        ImGui::SameLine(150);
        ImGui::TextColored(SignalColor(s.signal), "%+.3f", s.signal);
        ImGui::SameLine(220);
        Spark("sig", s.sigSeries, ImVec2(220, 30),
            IM_COL32(120, 200, 255, 255), true);
        ImGui::SameLine();
        ImGui::TextDisabled("imb %+.2f  cxl %+.2f", s.imbalanceClean, s.cancelAsym);
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1, 1), "OUT-OF-SAMPLE SCOREBOARD (live, forward)");

    if (ImGui::BeginTable("eval", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Horizon");
        ImGui::TableSetupColumn("Obs");
        ImGui::TableSetupColumn("Mean signed ret (bp)");
        ImGui::TableSetupColumn("t-stat");
        ImGui::TableSetupColumn("Hit rate");
        ImGui::TableHeadersRow();

        auto row = [&](const char* h, RunStat& r, double hit)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", h);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", (long long)r.n);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(SignalColor(r.mean), "%+.3f", r.mean);
            ImGui::TableSetColumnIndex(3);
            double t = r.T();
            ImGui::TextColored(std::fabs(t) > 2.0 ? ImVec4(0.3f, 1, 0.5f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                "%.2f", t);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f%%", hit * 100.0);
        };
        row("+5s", g_eval.ret5s, g_eval.HitRate5());
        row("+30s", g_eval.ret30s, g_eval.HitRate30());
        ImGui::EndTable();
    }

    if (g_eval.ret5s.n < 200)
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1),
            "Not enough observations yet (%lld). Do not trade this.", (long long)g_eval.ret5s.n);
    else if (std::fabs(g_eval.ret5s.T()) < 2.0)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1),
            "No edge detected at +5s. The weights below are not working on this tape.");
    else
        ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1),
            "Edge present at +5s (t=%.2f). Keep watching - it can decay intraday.",
            g_eval.ret5s.T());

    ImGui::Separator();
    ImGui::Text("Component weights");
    ImGui::SliderFloat("MM fair drift (IC-weighted)", &g_wFair, 0.0f, 2.0f);
    ImGui::SliderFloat("Book imbalance (spoof-cleaned)", &g_wImbal, 0.0f, 2.0f);
    ImGui::SliderFloat("Cancellation asymmetry", &g_wCxl, 0.0f, 2.0f);
    ImGui::SliderFloat("Adverse selection direction", &g_wMarkout, 0.0f, 2.0f);
    ImGui::SliderFloat("Cross-venue lead", &g_wLead, 0.0f, 2.0f);
    if (ImGui::Button("Reset scoreboard"))
    {
        g_eval.ret5s.Reset(); g_eval.ret30s.Reset();
        g_eval.hits5 = g_eval.tot5 = g_eval.hits30 = g_eval.tot30 = 0;
        g_eval.q5.clear(); g_eval.q30.clear();
    }
    ImGui::End();
}

// ============================================================================
//  19. MAIN
// ============================================================================

static void ConfigureDefaultStreams()
{
    // Same asset across venues: this is what makes cross-venue lead-lag work,
    // and it fits inside the usual 3-depth-line retail limit.
    struct { const char* sym; const char* ven; } def[] =
    {
        { "ALLW", "ISLAND" },   // NASDAQ TotalView - the only venue with real MPIDs
        { "ALLW", "ARCA"   },
        { "ALLW", "BATS"   },
    };

    g_streamCount = (int)(sizeof(def) / sizeof(def[0]));
    for (int i = 0; i < g_streamCount; ++i)
    {
        Stream& s = g_streams[i];
        s.id = i;
        snprintf(s.cfg.symbol, sizeof(s.cfg.symbol), "%s", def[i].sym);
        snprintf(s.cfg.venue, sizeof(s.cfg.venue), "%s", def[i].ven);
        s.cfg.tick = 0.01;
        s.cfg.enabled = true;
        s.prof.resize(kMaxMM);
        s.grid.tick = s.cfg.tick;
        s.rateWindowStart = NowNs();
    }
}

static void Subscribe(EClientSocket* client)
{
    g_tapeSymbols.clear();

    for (int i = 0; i < g_streamCount; ++i)
    {
        Stream& s = g_streams[i];
        if (!s.cfg.enabled) continue;

        Contract c;
        c.symbol = s.cfg.symbol;
        c.secType = "STK";
        c.exchange = s.cfg.venue;
        c.primaryExchange = s.cfg.primary;
        c.currency = "USD";

        client->reqMktDepth(1000 + i, c, 50, TagValueListSPtr());

        // The trade tape, once per distinct symbol. Without prints, a cancel
        // cannot be told from a fill and every cancellation / adverse-selection
        // number on this dashboard is noise.
        bool haveTape = false;
        for (auto& sym : g_tapeSymbols) if (sym == s.cfg.symbol) { haveTape = true; break; }
        if (!haveTape)
        {
            Contract t = c;
            t.exchange = "SMART";
            client->reqTickByTickData(2000 + (int)g_tapeSymbols.size(), t, "AllLast", 0, false);
            g_tapeSymbols.push_back(s.cfg.symbol);
        }
    }
    client->reqMktDepthExchanges();
}

int main()
{
    ConfigureDefaultStreams();

    EReaderOSSignal* signal = new EReaderOSSignal(2000);
    AlphaWrapper*    wrapper = new AlphaWrapper();
    EClientSocket*   client = new EClientSocket(wrapper, signal);

    bool connected = client->eConnect("127.0.0.1", 4001, 1232, false);
    LogLine(connected ? "connected to 127.0.0.1:4001" : "eConnect FAILED (is the gateway up?)");

    EReader* reader = nullptr;
    if (connected)
    {
        reader = new EReader(client, signal);
        reader->start();

        int64_t waitStart = NowNs();
        while (!wrapper->apiReady && (NowNs() - waitStart) < 10ll * 1000000000ll)
            reader->processMsgs();

        if (wrapper->apiReady)
        {
            client->reqMarketDataType(1);
            Subscribe(client);
        }
        else LogLine("timed out waiting for nextValidId");
    }
    else
    {
        // Nothing to connect to: fall straight into simulation so the analytics
        // and the UI are still exercisable.
        g_simMode = true;
    }

    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1760, 1000, "MM-ALPHA  -  Market Maker Behaviour", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 3.0f;
    ImGui::GetStyle().FrameRounding = 2.0f;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    int64_t lastTick = NowNs();

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Drain the socket with a frame budget so a burst cannot stall the UI.
        if (client->isConnected())
        {
            int64_t budgetEnd = NowNs() + 6ll * 1000000ll;
            do { reader->processMsgs(); } while (NowNs() < budgetEnd);
        }

        int64_t now = NowNs();

        if (g_simMode)
            for (int i = 0; i < g_streamCount; ++i) SimTick(g_streams[i], g_sim[i], now);

        if (now - lastTick >= 100000000ll) { Tick100ms(now); lastTick = now; }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        PanelStreams(client);
        PanelBookmap();
        PanelScorecard();
        PanelManipulation();
        PanelAnonymous();
        PanelLeadLag();
        PanelSignal();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.04f, 0.04f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    RecClose();
    if (client->isConnected()) client->eDisconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
