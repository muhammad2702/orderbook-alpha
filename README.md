# orderbook-alpha

**A real-time market-microstructure research terminal, in C++20, on live IBKR Level-2 depth.**

It reconstructs the order book exactly as the exchange protocol defines it, attributes every
add / modify / cancel / fill to the market-maker identity that caused it, and then asks one
question of each participant: *does watching this firm tell you where price is about to go?*

The answer is not asserted. It is scored against realised forward returns, live, on screen, and
the program is perfectly willing to tell you that the signal is worthless.

![Bookmap heatmap and feed health](docs/img/01-bookmap-heatmap.png)

---


## Why this exists

Most order-book visualisers show you *size at price*. That is the aggregate. It throws away the
one field that a Level-2 feed gives you and a Level-1 feed does not: **who posted the order**.

On NASDAQ TotalView every displayed order carries an MPID , the four-letter code of the firm that
posted it. Firms behave differently, and those differences are measurable and persistent. A
patient two-sided market maker is not the same object as a sub-50ms flicker quoter, which is not
the same object as an iceberg refilling the same price level, which is not the same object as
size posted three ticks off the touch and pulled the instant the tape moves.

This program builds a behavioural profile per firm per venue, in real time, and then tests
whether any of those profiles lead the mid price. That test is the whole point. Everything else
is instrumentation.



## Architecture

Data flows exactly one way. There are no back-edges, which is what makes the whole pipeline
testable from a file.

```
   IBKR EReader callbacks          Synthetic feed (market closed)
              │                              │
              └──────────────┬───────────────┘
                             ▼
                          Ladder            protocol-correct book, per stream
                             ▼
                          Event             {ts, stream, mm, side, kind, px, sz, life, depth}
                             ├──────────►  Recorder        CSV to disk, for replay / offline research
                             ▼
                        Analytics           per-MM profiles · spoof scoring · markouts · lead-lag
                             ▼
                          Signal            composite, IC-weighted
                             ▼
                      Live evaluation       realised forward return, hit rate, t-stat
                             ▼
                            UI              Dear ImGui / GLFW / OpenGL
```




## Statistical Findings


**Any dashboard can render a confident-looking number.** The Signal panel therefore logs every
signal observation alongside the realised forward return and accumulates the hit rate and t-stat
in real time. If the t-stat does not clear ~2, nothing else on the screen is tradeable, and the
panel says so rather than hiding it.


**Cross-venue lead-lag is the cleanest structural edge in here**, and the only one that doesn't
depend on a behavioural assumption: the same asset cannot be in two places at once, so if
venue A's mid reliably leads venue B's at some lag, venue B has to catch up. The program
subscribes to the same symbol on ISLAND / ARCA / BATS and measures the lagged cross-correlation
directly.

---

## Screenshots



![Bookmap](docs/img/01-bookmap-heatmap.png)



![Scorecard](docs/img/02-mm-scorecard.png)



![Anonymous and layering](docs/img/03-anonymous-and-layering.png)


![IBKR Gateway](docs/img/04-ibkr-gateway-live.png)

---

## Building

Windows / MSVC / CMake. Verified against Visual Studio 2026 (MSVC 19.51), CMake 4.4, x64 Release.

### 1. Get the IBKR TWS API

It is licensed by Interactive Brokers and **cannot be redistributed**, so it is not in this
repository. Download the C++ API from <https://interactivebrokers.github.io/> and unpack it so
that `TWS/cpp/client/EClientSocket.cpp` exists — or point CMake at wherever you put it:

```bash
cmake -S . -B build -A x64 -DTWS_API_DIR="C:/path/to/IBJts/source/cppclient/client"
```

### 2. Configure and build

GLFW 3.4 and Dear ImGui are fetched automatically by CMake. Nothing else is needed.



---

## What's next


**A statistical-arbitrage engine on the same IBKR stack.** Separately in progress: runtime
universe construction via IBKR's market scanner, dual-class pair detection from issuer names
(rather than a hardcoded ticker list), Augmented Dickey-Fuller cointegration testing with a
multiple-testing correction, Ornstein-Uhlenbeck fits for half-life and z-score, GARCH(1,1) as a
volatility filter, and quarter-Kelly sizing floored by CPPI with a drawdown circuit breaker.
It shares this repository's discipline about what a backtest is allowed to claim.

---

