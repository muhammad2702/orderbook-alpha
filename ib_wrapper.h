#pragma once
// ============================================================================
//  IbWrapperBase — the boring half of the IBKR EWrapper interface.
// ============================================================================
//
//  IB's EWrapper is a ~140-method pure-virtual interface. DefaultEWrapper
//  already supplies concrete no-ops for all of it, so this class exists only
//  to hold the handful of callbacks that are the same for every program built
//  on top of this API: connection handshake, error triage, contract lookup.
//
//  The interesting callbacks — updateMktDepth / updateMktDepthL2 — are
//  deliberately left as no-ops here. They are overridden in mm_alpha.cpp by
//  AlphaWrapper, which feeds them into the protocol-correct ladder. Putting a
//  second, independent book implementation in the base class is how you end up
//  with two versions of the same state machine disagreeing with each other.
// ============================================================================

#include <DefaultEWrapper.h>
#include <EReaderSignal.h>
#include <EReaderOSSignal.h>
#include "EClientSocket.h"

#include <string>

class IbWrapperBase : public DefaultEWrapper {
public:
    bool apiReady = false;     // set true on nextValidId — the real "connected"
    int  orderIdmine = 0;

    IbWrapperBase();

    // --- connection ---------------------------------------------------------
    void nextValidId(OrderId orderId);
    void currentTime(long time);

    // --- errors -------------------------------------------------------------
    // IB has shipped both signatures across API vintages; provide both so a
    // signature slip cannot silently leave the class abstract.
    void error(int id, int errorCode, const std::string& errorString);
    void error(int id, int errorCode, const std::string& errorString,
               const std::string& advancedOrderRejectJson);

    // --- market data (unused at this level) ---------------------------------
    void tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attribs);
    void tickSize(TickerId tickerId, TickType field, int size);
    void tickString(TickerId tickerId, TickType field, const std::string& value);

    // --- contract validation ------------------------------------------------
    void contractDetails(int reqId, const ContractDetails& contractDetails);
    void contractDetailsEnd(int reqId);

    // --- historical data ----------------------------------------------------
    void historicalData(TickerId reqId, const Bar& bar);
    void historicalDataEnd(int reqId, const std::string& startDateStr, const std::string& endDateStr);

    // --- depth: no-ops on purpose, see the note at the top of this file -----
    void updateMktDepth(TickerId id, int position, int operation, int side,
                        double price, int size);
    void updateMktDepthL2(TickerId id, int position, const std::string& marketMaker,
                          int operation, int side, double price, int size) override;
};
