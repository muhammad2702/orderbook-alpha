#include "ib_wrapper.h"

#include <cstdio>
#include <iostream>

IbWrapperBase::IbWrapperBase()
{
    std::cout << "[wrapper] constructed\n";
}

// ============================================================================
//  CONNECTION
// ============================================================================
//
//  eConnect() returning true only means a socket opened. The API is not
//  actually usable until the server sends nextValidId, so that — not the
//  return value of eConnect — is what the main loop waits on.
// ============================================================================

void IbWrapperBase::nextValidId(OrderId orderId)
{
    std::cout << "[connection] nextValidId = " << orderId << " -- API IS READY\n";
    orderIdmine = orderId;
    apiReady = true;
}

void IbWrapperBase::currentTime(long time)
{
    std::cout << "[connection] server epoch time: " << time << "\n";
}

// ============================================================================
//  ERRORS
// ============================================================================
//
//  IB pushes informational notices down the same channel as real errors. The
//  ones filtered below are status chatter that fires constantly on a healthy
//  connection; letting them through buries the two codes that actually matter:
//
//      309  too many simultaneous market-depth requests (account limit)
//      354  not subscribed to the market data / depth entitlement you asked for
//
//  Both are surfaced verbatim in the Streams panel rather than swallowed,
//  because a depth feed that silently returns nothing looks identical to a
//  quiet market.
// ============================================================================

void IbWrapperBase::error(int id, int errorCode, const std::string& errorString)
{
    switch (errorCode) {
    case 2104:  // market data farm connection is OK
    case 2106:  // historical data farm connection is OK
    case 2119:  // market data farm is connecting
    case 2158:  // sec-def data farm connection is OK
        return;
    default:
        break;
    }

    std::cout << "[error] reqId=" << id << " code=" << errorCode
              << " | " << errorString << "\n";
}

void IbWrapperBase::error(int id, int errorCode, const std::string& errorString,
                          const std::string& advancedOrderRejectJson)
{
    std::cout << "[error/advanced] reqId=" << id << " code=" << errorCode
              << " | " << errorString << "\n";
}

// ============================================================================
//  MARKET DATA  (top-of-book ticks are not used by this program)
// ============================================================================

void IbWrapperBase::tickPrice(TickerId, TickType, double, const TickAttrib&) {}
void IbWrapperBase::tickSize(TickerId, TickType, int) {}
void IbWrapperBase::tickString(TickerId, TickType, const std::string&) {}

// ============================================================================
//  CONTRACT VALIDATION
// ============================================================================
//
//  Worth logging: an ambiguous contract (a US symbol that also lists abroad)
//  is the most common reason a depth subscription returns nothing at all.
//  Setting primaryExchange on the Contract is what disambiguates it.
// ============================================================================

void IbWrapperBase::contractDetails(int reqId, const ContractDetails& contractDetails)
{
    std::cout << "[contract] reqId=" << reqId
              << " conId="   << contractDetails.contract.conId
              << " symbol="  << contractDetails.contract.symbol
              << " exchange=" << contractDetails.contract.exchange << "\n";
}

void IbWrapperBase::contractDetailsEnd(int reqId)
{
    std::cout << "[contract] end for reqId=" << reqId << "\n";
}

// ============================================================================
//  HISTORICAL DATA  (unused here; the analytics are all live-tick)
// ============================================================================

void IbWrapperBase::historicalData(TickerId, const Bar&) {}
void IbWrapperBase::historicalDataEnd(int, const std::string&, const std::string&) {}

// ============================================================================
//  DEPTH  —  intentionally empty at this level.
// ============================================================================
//
//  See the note in ib_wrapper.h. AlphaWrapper (mm_alpha.cpp) overrides both of
//  these and routes them into the position-indexed ladder, which is the only
//  place in this program that is allowed to hold order-book state.
// ============================================================================

void IbWrapperBase::updateMktDepth(TickerId, int, int, int, double, int) {}

void IbWrapperBase::updateMktDepthL2(TickerId, int, const std::string&,
                                     int, int, double, int) {}
