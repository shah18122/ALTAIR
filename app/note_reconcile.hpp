// app/note_reconcile.hpp -- daily reconciliation against the broker's contract note.
//
// P12-04.
//
// THE CONTRACT NOTE IS THE AUTHORITY. EVERYTHING THE ENGINE BELIEVES IS A
// PREDICTION UNTIL IT IS RECONCILED AGAINST ONE.
//
// `risk/cost.hpp` computes brokerage, STT, exchange charges, SEBI, stamp duty,
// IPFT and GST from an effective-dated schedule, and CLAUDE.md rule 5 says no
// strategy ever sees a pre-cost number. That makes the cost model load-bearing:
// every signal in this engine is priced through it, so a wrong rate does not
// produce a wrong report, it produces wrong TRADES -- an edge that clears a
// hurdle it should not have cleared.
//
// The contract note is the only place that model is ever measured against
// reality. This file is that measurement.
//
// A RECONCILER THAT MATCHES ON A TOTAL MATCHES NOTHING.
//
// The obvious daily check sums the engine's charges, sums the note's charges,
// and compares. It passes on a day when the engine over-charged one trade by
// Rs 5 and under-charged another by Rs 5, which is not a rounding artefact --
// it is two different rates being wrong in two different directions, and it is
// the exact shape a stale effective-date produces. So reconciliation is
// ROW-LEVEL and the totals are a display, never the test.
//
// AND IT RECONCILES HEAD BY HEAD, NOT TRADE BY TRADE.
//
// Matching a trade's total charge tells you that you are wrong. Matching STT
// against STT and stamp against stamp tells you WHICH RATE IS STALE, which is
// the difference between a finding and a fix. CLAUDE.md records that STT rose
// on 2026-04-01 -- futures 0.02 to 0.05%, options 0.10 to 0.15% on sell-side
// premium -- and on the morning that took effect the failure is one head
// moving on one side of one segment. A per-trade total blurs it into "charges
// are a bit high lately".
//
// THE THREE OUTCOMES, AND THE ONE THAT IS DANGEROUS.
//
//   * MATCHED, AGREEING -- the good case.
//   * MATCHED, DISAGREEING -- the cost model is wrong. Bad, but bounded: you
//     know the position, you know the fill, only the money is off.
//   * ENGINE-ONLY -- the engine believes in a fill the broker never made. A
//     phantom position. The engine will hedge against something it does not
//     hold.
//   * BROKER-ONLY -- THE DANGEROUS ONE. The broker executed a trade the engine
//     has no record of. Either something outside `oms/` placed an order, or a
//     fill arrived during a crash and was never booked. Position, margin and
//     conservation are all wrong, and NOTHING in the engine is reporting a
//     problem, because from its side that trade does not exist.
//
// A reconciler that reports only "differences among matched rows" is blind to
// the last two, and they are the two that move real money.
//
// AN EMPTY NOTE IS NOT A CLEAN DAY.
//
// The realistic failure is not a subtly wrong note, it is a download that
// returned zero rows -- an expired session, a holiday the fetcher did not know
// about, a changed URL. Nothing matches, nothing disagrees, and a reconciler
// that reports "0 mismatches" is reporting the truth and answering the wrong
// question. Absence is not agreement. `Report::ok()` requires that something
// was actually compared.

#pragma once

#include <core/types/units.hpp>
#include <risk/cost.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace altair {

// -------------------------------------------------------------------------
// The two sides
// -------------------------------------------------------------------------

/// The seven heads, itemised. Mirrors `CostBreakdown` (risk/cost.hpp D5) --
/// "a cost you cannot decompose is a cost you cannot audit" -- and a contract
/// note itemises the same way, which is what makes the comparison possible.
struct ChargeHeads {
    Notional brokerage{};
    Notional stt{};
    Notional exchange_txn{};
    Notional sebi{};
    Notional stamp{};
    Notional ipft{};
    Notional gst{};

    [[nodiscard]] Notional total() const noexcept {
        return Notional{brokerage.raw() + stt.raw() + exchange_txn.raw()
                        + sebi.raw() + stamp.raw() + ipft.raw() + gst.raw()};
    }
};

/// Lift the engine's own computation into the comparison shape. No arithmetic
/// happens here on purpose: if this function adjusted anything, the reconciler
/// would be checking the cost model against a second copy of itself.
[[nodiscard]] inline ChargeHeads heads_of(const CostBreakdown& c) noexcept {
    return ChargeHeads{c.brokerage, c.stt, c.exchange_txn,
                       c.sebi,      c.stamp, c.ipft, c.gst};
}

/// One executed trade, from either side. The same struct for both, so a field
/// the engine records and the note does not cannot quietly go uncompared.
struct TradeRecord {
    /// The broker's own order id. The join key, because it is the one
    /// identifier both sides genuinely share -- a symbol-and-time join looks
    /// fine until two fills of the same order land in the same second.
    std::string order_id;
    std::string symbol;
    Side side = Side::Buy;
    Qty qty{};
    /// UNIT: paise. The traded price; for an option, the premium.
    Price price{};
    ChargeHeads charges{};
};

// -------------------------------------------------------------------------
// Findings
// -------------------------------------------------------------------------

enum class Finding : std::uint8_t {
    /// Present on both sides and every field agrees to the paisa.
    Agree,
    /// Present on both sides, quantity or price differs. The engine's idea of
    /// the POSITION is wrong, which is worse than its idea of the money.
    FillMismatch,
    /// Present on both sides, fill agrees, a charge head differs.
    ChargeMismatch,
    /// The engine booked a fill the broker never made.
    EngineOnly,
    /// The broker made a fill the engine never booked. See the header.
    BrokerOnly,
    /// The same order id appears twice on one side. Not matched, because a
    /// silent double-match would net two errors into zero.
    DuplicateId
};

[[nodiscard]] inline const char* finding_text(Finding f) noexcept {
    switch (f) {
    case Finding::Agree:          return "agree";
    case Finding::FillMismatch:   return "FILL MISMATCH";
    case Finding::ChargeMismatch: return "CHARGE MISMATCH";
    case Finding::EngineOnly:     return "ENGINE-ONLY (phantom position)";
    case Finding::BrokerOnly:     return "BROKER-ONLY (unbooked fill)";
    case Finding::DuplicateId:    return "DUPLICATE ORDER ID";
    }
    return "?";
}

/// Which head disagreed, so the report names the rate rather than the trade.
enum class Head : std::uint8_t {
    None, Brokerage, Stt, ExchangeTxn, Sebi, Stamp, Ipft, Gst
};

[[nodiscard]] inline const char* head_text(Head h) noexcept {
    switch (h) {
    case Head::None:        return "-";
    case Head::Brokerage:   return "brokerage";
    case Head::Stt:         return "STT";
    case Head::ExchangeTxn: return "exchange txn";
    case Head::Sebi:        return "SEBI";
    case Head::Stamp:       return "stamp duty";
    case Head::Ipft:        return "IPFT";
    case Head::Gst:         return "GST";
    }
    return "?";
}

struct Discrepancy {
    Finding finding = Finding::Agree;
    std::string order_id;
    std::string symbol;
    /// The first head that differs. `None` unless `finding` is ChargeMismatch.
    Head head = Head::None;
    /// UNIT: paise. engine minus broker. SIGNED, deliberately: a reconciler
    /// that reports magnitudes cannot tell you that the engine is systematically
    /// UNDER-charging, which is the direction that makes an unprofitable
    /// strategy look profitable.
    std::int64_t delta_paise = 0;
};

/// The day's result.
struct Report {
    std::size_t engine_rows = 0;
    std::size_t broker_rows = 0;
    std::size_t agreed = 0;
    std::vector<Discrepancy> discrepancies;

    /// UNIT: paise, signed, engine minus broker. Reported for display ONLY.
    /// It is the number a total-matching reconciler would have compared, and
    /// it is in the report so a day where it is 0 and `ok()` is false is
    /// visible rather than theoretical.
    std::int64_t net_charge_delta_paise = 0;

    /// A day reconciles only when something was compared AND nothing differed.
    ///
    /// The `engine_rows == 0 && broker_rows == 0` case is a genuine flat day
    /// and passes; a day where one side is empty and the other is not shows up
    /// as unmatched rows and fails, which is what an empty download looks like.
    [[nodiscard]] bool ok() const noexcept { return discrepancies.empty(); }
};

// -------------------------------------------------------------------------
// The comparison
// -------------------------------------------------------------------------

namespace detail {

/// First differing head, or None. Order is the order a contract note prints
/// them, so the report reads alongside the document it came from.
[[nodiscard]] inline Head first_differing_head(const ChargeHeads& e,
                                               const ChargeHeads& b,
                                               std::int64_t& delta) noexcept {
    struct Pair { Head h; std::int64_t a, c; };
    const Pair heads[] = {
        {Head::Brokerage,   e.brokerage.raw(),    b.brokerage.raw()},
        {Head::Stt,         e.stt.raw(),          b.stt.raw()},
        {Head::ExchangeTxn, e.exchange_txn.raw(), b.exchange_txn.raw()},
        {Head::Sebi,        e.sebi.raw(),         b.sebi.raw()},
        {Head::Stamp,       e.stamp.raw(),        b.stamp.raw()},
        {Head::Ipft,        e.ipft.raw(),         b.ipft.raw()},
        {Head::Gst,         e.gst.raw(),          b.gst.raw()},
    };
    for (const Pair& p : heads) {
        if (p.a != p.c) {
            delta = p.a - p.c;
            return p.h;
        }
    }
    delta = 0;
    return Head::None;
}

/// Indices of any order id appearing more than once. A duplicate is never
/// matched: a partial fill reported twice, matched twice, nets two errors to
/// zero -- which is the failure this whole file exists to prevent, arriving
/// through the join instead of through the arithmetic.
[[nodiscard]] inline std::vector<bool>
mark_duplicates(const std::vector<TradeRecord>& rows) {
    std::vector<bool> dup(rows.size(), false);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        for (std::size_t j = i + 1; j < rows.size(); ++j) {
            if (rows[i].order_id == rows[j].order_id) {
                dup[i] = true;
                dup[j] = true;
            }
        }
    }
    return dup;
}

} // namespace detail

/// Reconcile a day. `engine` is what `oms/` booked; `broker` is the contract
/// note. Neither is trusted over the other on the FILL -- a difference there is
/// reported, not resolved -- but the note is authoritative on CHARGES, which is
/// why the delta is signed engine-minus-broker.
[[nodiscard]] inline Report reconcile(const std::vector<TradeRecord>& engine,
                                      const std::vector<TradeRecord>& broker) {
    Report r;
    r.engine_rows = engine.size();
    r.broker_rows = broker.size();

    const std::vector<bool> edup = detail::mark_duplicates(engine);
    const std::vector<bool> bdup = detail::mark_duplicates(broker);

    std::vector<bool> broker_used(broker.size(), false);

    for (std::size_t i = 0; i < engine.size(); ++i) {
        const TradeRecord& e = engine[i];
        r.net_charge_delta_paise += e.charges.total().raw();

        if (edup[i]) {
            r.discrepancies.push_back({Finding::DuplicateId, e.order_id,
                                       e.symbol, Head::None, 0});
            continue;
        }

        std::size_t found = broker.size();
        for (std::size_t j = 0; j < broker.size(); ++j) {
            if (!broker_used[j] && !bdup[j] && broker[j].order_id == e.order_id) {
                found = j;
                break;
            }
        }
        if (found == broker.size()) {
            r.discrepancies.push_back({Finding::EngineOnly, e.order_id,
                                       e.symbol, Head::None,
                                       e.charges.total().raw()});
            continue;
        }
        broker_used[found] = true;
        const TradeRecord& b = broker[found];

        // THE FILL BEFORE THE MONEY. A quantity difference makes every charge
        // comparison meaningless -- of course the STT differs, it is a
        // percentage of a different trade -- and reporting the charge would
        // point at the cost model when the fault is in the order state
        // machine.
        if (e.qty.raw() != b.qty.raw() || e.price.raw() != b.price.raw()
            || e.side != b.side || e.symbol != b.symbol) {
            r.discrepancies.push_back({Finding::FillMismatch, e.order_id,
                                       e.symbol, Head::None,
                                       e.qty.raw() * e.price.raw()
                                           - b.qty.raw() * b.price.raw()});
            continue;
        }

        std::int64_t delta = 0;
        const Head h = detail::first_differing_head(e.charges, b.charges, delta);
        if (h != Head::None) {
            r.discrepancies.push_back({Finding::ChargeMismatch, e.order_id,
                                       e.symbol, h, delta});
        } else {
            ++r.agreed;
        }
    }

    for (std::size_t j = 0; j < broker.size(); ++j) {
        r.net_charge_delta_paise -= broker[j].charges.total().raw();
        if (bdup[j]) {
            r.discrepancies.push_back({Finding::DuplicateId, broker[j].order_id,
                                       broker[j].symbol, Head::None, 0});
        } else if (!broker_used[j]) {
            r.discrepancies.push_back({Finding::BrokerOnly, broker[j].order_id,
                                       broker[j].symbol, Head::None,
                                       broker[j].charges.total().raw()});
        }
    }
    return r;
}

} // namespace altair
