// app/tests/test_note_reconcile.cpp -- P12-04.
//
// The engine's cost model is a prediction. The contract note is the fact. This
// test is the measurement, and it is driven by the REAL `compute_cost` rather
// than by hand-typed charge figures -- a reconciler tested only against
// invented numbers proves that subtraction works.
//
// The headline case is the one CLAUDE.md warns about by name: STT rose on
// 2026-04-01 and every pre-April backtest is optimistic until re-run. The
// morning that took effect, an engine still holding the old schedule produces
// a plausible number for every trade and the wrong number for exactly one head
// on exactly one side. A total-matching reconciler sees "charges a bit high";
// this one names STT.

#include <app/note_reconcile.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

using altair::BrokerageRule;
using altair::ChargeSchedule;
using altair::ChargeSide;
using altair::CostBreakdown;
using altair::Discrepancy;
using altair::Exchange;
using altair::Finding;
using altair::Head;
using altair::Notional;
using altair::Price;
using altair::Qty;
using altair::Report;
using altair::Segment;
using altair::Side;
using altair::Timestamp;
using altair::TradeRecord;
using altair::TurnoverBasis;
using altair::rate_from;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

constexpr altair::Duration kIstOffset = altair::duration::minutes(330);

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m,
                                       unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned shifted = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * shifted + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr Timestamp ist_date(std::int64_t y, unsigned m, unsigned d) noexcept {
    return Timestamp{days_from_civil(y, m, d) * 86'400'000'000'000LL
                     - kIstOffset.raw()};
}

// The two deterministic schedule fixtures, same as risk/tests/test_cost.cpp.
// Their values are known test inputs only; `verified` lets compute_cost
// exercise the fixture arithmetic and does NOT verify the production TOML
// schedule, which remains fail-closed until its rates have a cited source.
// Duplicated rather than shared because a reconciler that imports the cost
// model's own test fixtures is checking the model against itself.
ChargeSchedule pre_april() {
    ChargeSchedule s{};
    s.valid_from = ist_date(2000, 1, 1);
    s.valid_to   = ist_date(2026, 3, 31);
    s.verified   = true;

    s.equity_futures.present      = true;
    s.equity_futures.stt          = rate_from(0.0002L);      // 0.02%
    s.equity_futures.stt_side     = ChargeSide::Sell;
    s.equity_futures.exch_txn_nse = rate_from(0.0000173L);
    s.equity_futures.stamp        = rate_from(0.00002L);
    s.equity_futures.ipft         = rate_from(0.0000005L);

    s.equity_options.present      = true;
    s.equity_options.basis        = TurnoverBasis::Premium;
    s.equity_options.stt          = rate_from(0.001L);       // 0.10%
    s.equity_options.stt_side     = ChargeSide::Sell;
    s.equity_options.exch_txn_nse = rate_from(0.0003503L);
    s.equity_options.stamp        = rate_from(0.00003L);
    s.equity_options.ipft         = rate_from(0.000005L);
    return s;
}

ChargeSchedule post_april() {
    ChargeSchedule s = pre_april();
    s.valid_from = ist_date(2026, 4, 1);
    s.valid_to   = ist_date(2099, 12, 31);
    s.equity_futures.stt = rate_from(0.0005L);               // 0.02% -> 0.05%
    s.equity_options.stt = rate_from(0.0015L);               // 0.10% -> 0.15%
    return s;
}

BrokerageRule zerodha_fo() {
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};
    b.pct = rate_from(0.0003L);
    b.take_lower = true;
    return b;
}

/// A NIFTY futures trade priced through the REAL cost model.
TradeRecord booked(const char* id, Side side, std::int64_t px, std::int64_t qty,
                   const ChargeSchedule& sch) {
    altair::Trade t{};
    t.segment = Segment::Fut;
    t.exchange = Exchange::NSE;
    t.side = side;
    t.qty = Qty{qty};
    t.price = Price{px};
    t.trade_ts = ist_date(2026, 4, 1);
    const auto c = altair::compute_cost(t, sch, zerodha_fo());
    if (!c) { std::printf("  FATAL: cost model refused\n"); std::exit(2); }

    TradeRecord r;
    r.order_id = id;
    r.symbol = "NIFTY26APRFUT";
    r.side = side;
    r.qty = t.qty;
    r.price = t.price;
    r.charges = altair::heads_of(*c);
    return r;
}

void dump(const Report& r) {
    std::printf("        %zu engine / %zu broker rows, %zu agreed, "
                "net delta %+lld p\n",
                r.engine_rows, r.broker_rows, r.agreed,
                static_cast<long long>(r.net_charge_delta_paise));
    for (const Discrepancy& d : r.discrepancies) {
        std::printf("        %-30s %-14s %-13s %+lld p\n",
                    altair::finding_text(d.finding), d.order_id.c_str(),
                    altair::head_text(d.head),
                    static_cast<long long>(d.delta_paise));
    }
}

} // namespace

int main() {
    std::printf("P12-04 daily reconciliation against the contract note\n");

    const ChargeSchedule old_sch = pre_april();
    const ChargeSchedule cur_sch = post_april();

    // ---- 1. A CLEAN DAY ---------------------------------------------------
    {
        std::vector<TradeRecord> e{
            booked("O1", Side::Buy,  2'408'000, 65, cur_sch),
            booked("O2", Side::Sell, 2'415'000, 65, cur_sch)};
        const std::vector<TradeRecord> b = e;
        const Report r = altair::reconcile(e, b);
        check(r.ok() && r.agreed == 2,
              "a day where both sides used the same schedule reconciles, and "
              "it reconciles to the PAISA -- the cost model already rounds "
              "head by head the way a note does");
    }

    // ---- 2. THE 1 APRIL STT RISE ------------------------------------------
    //
    // The engine is still on the pre-April schedule. Every number it produces
    // is plausible. Exactly one head, on exactly one side, is wrong.
    {
        const std::vector<TradeRecord> e{
            booked("O1", Side::Buy,  2'408'000, 65, old_sch),
            booked("O2", Side::Sell, 2'415'000, 65, old_sch)};
        const std::vector<TradeRecord> b{
            booked("O1", Side::Buy,  2'408'000, 65, cur_sch),
            booked("O2", Side::Sell, 2'415'000, 65, cur_sch)};
        const Report r = altair::reconcile(e, b);
        dump(r);

        check(!r.ok() && r.discrepancies.size() == 1
                  && r.discrepancies[0].head == Head::Stt,
              "a stale schedule is reported as ONE head on ONE trade, named: "
              "STT. Not 'charges look high' -- the rate to fix");
        check(r.discrepancies[0].order_id == "O2"
                  && r.discrepancies[0].delta_paise < 0,
              "on the SELL only, because futures STT is sell-side, and NEGATIVE"
              " -- the engine UNDER-charged, which is the direction that makes "
              "an unprofitable trade look profitable");
        check(r.agreed == 1,
              "and the buy leg still agrees, which is what makes the finding "
              "specific instead of a day-wide shrug");
    }

    // ---- 3. TWO ERRORS THAT CANCEL ----------------------------------------
    //
    // The case that motivates row-level reconciliation. A total-matching check
    // compares one number to one number and both are equal.
    {
        std::vector<TradeRecord> e{
            booked("O1", Side::Buy,  2'408'000, 65, cur_sch),
            booked("O2", Side::Sell, 2'415'000, 65, cur_sch)};
        const std::vector<TradeRecord> b = e;
        e[0].charges.brokerage = Notional{e[0].charges.brokerage.raw() + 500};
        e[1].charges.brokerage = Notional{e[1].charges.brokerage.raw() - 500};

        const Report r = altair::reconcile(e, b);
        dump(r);
        check(r.net_charge_delta_paise == 0,
              "the day's NET charge difference is exactly zero -- a "
              "total-matching reconciler passes this day");
        check(!r.ok() && r.discrepancies.size() == 2
                  && r.discrepancies[0].head == Head::Brokerage
                  && r.discrepancies[1].head == Head::Brokerage,
              "and row-level finds BOTH, +Rs 5 and -Rs 5: not rounding, two "
              "rates wrong in two directions");
    }

    // ---- 4. THE BROKER TRADED SOMETHING THE ENGINE NEVER BOOKED -----------
    //
    // The dangerous one. Position, margin and conservation are all wrong and
    // nothing in the engine is reporting a problem, because from its side that
    // trade does not exist.
    {
        const std::vector<TradeRecord> e{
            booked("O1", Side::Buy, 2'408'000, 65, cur_sch)};
        const std::vector<TradeRecord> b{
            booked("O1", Side::Buy,  2'408'000, 65, cur_sch),
            booked("O9", Side::Sell, 2'420'000, 65, cur_sch)};
        const Report r = altair::reconcile(e, b);
        dump(r);
        check(!r.ok() && r.discrepancies.size() == 1
                  && r.discrepancies[0].finding == Finding::BrokerOnly
                  && r.discrepancies[0].order_id == "O9",
              "a broker fill with no engine record is reported as BROKER-ONLY "
              "and named -- a reconciler that only diffs MATCHED rows is blind "
              "to it, and it is the one that moves real money");
    }

    // ---- 5. AND THE PHANTOM IN THE OTHER DIRECTION ------------------------
    {
        const std::vector<TradeRecord> e{
            booked("O1", Side::Buy,  2'408'000, 65, cur_sch),
            booked("O7", Side::Sell, 2'411'000, 65, cur_sch)};
        const std::vector<TradeRecord> b{
            booked("O1", Side::Buy, 2'408'000, 65, cur_sch)};
        const Report r = altair::reconcile(e, b);
        check(!r.ok() && r.discrepancies.size() == 1
                  && r.discrepancies[0].finding == Finding::EngineOnly,
              "and an engine fill the broker never made is a PHANTOM POSITION "
              "-- the engine will hedge against something it does not hold");
    }

    // ---- 6. THE FILL BEFORE THE MONEY -------------------------------------
    //
    // A quantity difference makes every charge comparison meaningless: of
    // course the STT differs, it is a percentage of a different trade.
    // Reporting the charge would point at the cost model when the fault is in
    // the order state machine.
    {
        const std::vector<TradeRecord> e{
            booked("O1", Side::Buy, 2'408'000, 130, cur_sch)};   // 2 lots
        const std::vector<TradeRecord> b{
            booked("O1", Side::Buy, 2'408'000, 65,  cur_sch)};   // 1 filled
        const Report r = altair::reconcile(e, b);
        check(r.discrepancies.size() == 1
                  && r.discrepancies[0].finding == Finding::FillMismatch,
              "a partial fill is a FILL MISMATCH, not a charge mismatch -- the "
              "charges differ too, and blaming the cost model would send the "
              "fix to the wrong module");
    }

    // ---- 7. AN EMPTY NOTE IS NOT A CLEAN DAY ------------------------------
    //
    // The realistic failure: an expired session, a holiday the fetcher did not
    // know about, a changed URL. Zero rows come back and nothing mismatches.
    {
        const std::vector<TradeRecord> e{
            booked("O1", Side::Buy,  2'408'000, 65, cur_sch),
            booked("O2", Side::Sell, 2'415'000, 65, cur_sch)};
        const Report empty_note = altair::reconcile(e, {});
        check(!empty_note.ok() && empty_note.discrepancies.size() == 2,
              "a note that downloaded zero rows against a day that traded is "
              "TWO unmatched fills, not '0 mismatches' -- absence is not "
              "agreement");

        const Report flat_day = altair::reconcile({}, {});
        check(flat_day.ok(),
              "while a genuinely flat day -- nothing on either side -- does "
              "reconcile, so the check above is not just 'empty always fails'");
    }

    // ---- 8. A PARTIAL FILL REPORTED TWICE ---------------------------------
    //
    // A duplicate id matched twice nets two errors to zero: the same failure
    // as case 3, arriving through the join instead of the arithmetic.
    {
        const std::vector<TradeRecord> e{
            booked("O1", Side::Buy, 2'408'000, 65, cur_sch)};
        std::vector<TradeRecord> b{
            booked("O1", Side::Buy, 2'408'000, 65, cur_sch),
            booked("O1", Side::Buy, 2'408'000, 65, cur_sch)};
        const Report r = altair::reconcile(e, b);
        bool saw_dup = false;
        for (const Discrepancy& d : r.discrepancies) {
            if (d.finding == Finding::DuplicateId) { saw_dup = true; }
        }
        check(!r.ok() && saw_dup && r.agreed == 0,
              "an order id appearing twice on the note is refused, not matched "
              "-- a silent double-match is a second way to net two errors "
              "into zero");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
