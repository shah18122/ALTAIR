// P6-05 acceptance tests for backtest/engine.hpp.
//
// Test 1 is the card: the fill model IS the backtest. The same strategy over
// the same tape, filled at the mid and filled at the touch, and the difference
// measured in rupees rather than argued about.
//
// Test 2 is the other half: a limit order does not fill because the price
// touched it. The queue-aware model and the naive one are run over the same
// prints and their fill rates compared.
//
// Test 3 is the conservation identity, and test 4 is that the strategy has no
// argument through which a future tick could arrive.
//
// No check description here may contain the substring FAIL.

#include <backtest/engine.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

double rupees(std::int64_t p) { return static_cast<double>(p) / 100.0; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

ChargeSchedule schedule()
{
    ChargeSchedule s{};
    s.valid_from = Timestamp{0};
    s.valid_to = Timestamp{std::numeric_limits<std::int64_t>::max()};
    s.verified = true;
    s.equity_intraday.present   = true;
    s.equity_intraday.stt       = rate_from(0.00025L);
    s.equity_intraday.stt_side  = ChargeSide::Sell;
    s.equity_intraday.exch_txn_nse = rate_from(0.0000297L);
    s.equity_intraday.stamp     = rate_from(0.00003L);
    s.equity_intraday.stamp_side = ChargeSide::Buy;
    s.equity_intraday.ipft      = rate_from(0.000001L);
    return s;
}

BrokerageRule broker()
{
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};
    b.pct = rate_from(0.0003L);
    b.take_lower = true;
    return b;
}

constexpr std::size_t kN = 20'000;
Tick tape[kN];
Quote quotes[kN];

/// A NIFTY-scale tape with a 5-paise half-spread -- a TIGHT market, so the
/// measurement below is a lower bound on what a wide one would show.
void make_tape(std::uint64_t seed, std::int64_t half_spread)
{
    Lcg g{seed};
    double p = 2'400'000.0;
    for (std::size_t i = 0; i < kN; ++i) {
        p += g.normal() * 400.0;
        const std::int64_t mid = static_cast<std::int64_t>(p);
        tape[i] = Tick{};
        tape[i].exchange_ts = Timestamp{static_cast<std::int64_t>(i) * 1000};
        tape[i].last = Price{mid};
        tape[i].last_qty = Qty{25};
        quotes[i].bid = Price{mid - half_spread};
        quotes[i].ask = Price{mid + half_spread};
        quotes[i].bid_qty = Qty{900};
        quotes[i].ask_qty = Qty{900};
    }
}

/// A MEAN-REVERTING tape: an OU process around 24,000 with a 30-paise shock.
///
/// Needed for the queue test and not for the fill-model one. A random walk of
/// 400-paise steps blows straight through a limit 20 paise away within three
/// ticks, so the order trades THROUGH and its queue position never binds --
/// which measures the trade-through path rather than the queue. A book that
/// oscillates around a level is where queue position is the whole story, and
/// it is also what a quote-driven market actually looks like intraday.
void make_reverting_tape(std::uint64_t seed, std::int64_t half_spread)
{
    Lcg g{seed};
    double p = 2'400'000.0;
    for (std::size_t i = 0; i < kN; ++i) {
        p += 0.05 * (2'400'000.0 - p) + g.normal() * 30.0;
        // QUANTISED to the 5-paise tick. Real prices live on a grid, and the
        // grid is why "a print AT my limit" is a common event rather than a
        // measure-zero one -- which is what makes queue position bind at all.
        const std::int64_t mid =
            5 * static_cast<std::int64_t>(p / 5.0 + 0.5);
        tape[i] = Tick{};
        tape[i].exchange_ts = Timestamp{static_cast<std::int64_t>(i) * 1000};
        tape[i].last = Price{mid};
        tape[i].last_qty = Qty{25};
        quotes[i].bid = Price{mid - half_spread};
        quotes[i].ask = Price{mid + half_spread};
        quotes[i].bid_qty = Qty{900};
        quotes[i].ask_qty = Qty{900};
    }
}

/// A simple alternating strategy: trade every `period` ticks. Deliberately
/// EDGELESS -- it has no signal at all, so whatever P&L appears is entirely
/// the fill model's doing.
struct Metronome {
    std::size_t period;
    std::size_t i = 0;
    bool buy_next = true;
    Intent on_tick(const Tick&, const Quote&, const Ledger&) noexcept {
        Intent in{};
        if (++i % period == 0) {
            in.act = true;
            in.side = buy_next ? Side::Buy : Side::Sell;
            in.qty = Qty{75};
            buy_next = !buy_next;
        }
        return in;
    }
};

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void the_fill_model_is_the_backtest()
{
    std::printf("\n1 the_fill_model_is_the_backtest\n");
    make_tape(0xF111, 5);               // 5-paise half-spread: a tight market
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = broker();

    auto go = [&](FillModel m) {
        Metronome strat{50};
        Ledger led{Notional{10'000'000}};       // Rs 1,00,000
        return std::pair<std::expected<BacktestResult, BacktestError>, Ledger>{
            run(tape, quotes, kN, strat, led, m, sch, br, 0.0, Segment::Cash,
                Exchange::NSE),
            led};
    };

    const auto mid = go(FillModel::Mid);
    const auto touch = go(FillModel::Touch);
    check(mid.first.has_value() && touch.first.has_value(),
          "both runs complete");
    if (!mid.first || !touch.first) { return; }

    const double gift = rupees(mid.first->pnl.raw() - touch.first->pnl.raw());
    std::printf("    the SAME edgeless strategy over the SAME %zu ticks,"
                " %zu fills each,\n    on a market quoted just 10 paise wide:\n"
                "      filled at the MID     P&L Rs %9.2f   costs Rs %8.2f\n"
                "      filled at the TOUCH   P&L Rs %9.2f   costs Rs %8.2f\n"
                "      the mid model is worth Rs %.2f, or Rs %.2f per fill\n",
                kN, mid.first->fills,
                rupees(mid.first->pnl.raw()), rupees(mid.first->costs.raw()),
                rupees(touch.first->pnl.raw()), rupees(touch.first->costs.raw()),
                gift,
                gift / static_cast<double>(mid.first->fills));

    check(mid.first->fills == touch.first->fills,
          "both models take exactly the same trades -- only the price differs,"
          " which is what makes the gap the fill model and nothing else");
    check(gift > 0.0,
          "and filling at the mid is worth real money to a strategy with no"
          " edge whatsoever");
    check(std::fabs(gift / static_cast<double>(mid.first->fills) - 3.75) < 1.0,
          "about Rs 3.75 a fill, which is the 5-paise half-spread times the"
          " 75-unit lot: the model hands back exactly the spread it declined"
          " to cross");
    std::printf("    -> and this is a TIGHT market. The gift scales with the"
                " spread and with the\n       turnover, so the strategies most"
                " flattered by it are the high-frequency ones\n       that look"
                " best on a mid-filled backtest.\n");

    // Impact makes it worse still, which is the honest direction.
    Metronome s3{50};
    Ledger l3{Notional{10'000'000}};
    const auto imp = run(tape, quotes, kN, s3, l3, FillModel::TouchPlusImpact,
                         sch, br, 12.0, Segment::Cash, Exchange::NSE);
    check(imp.has_value() && imp->pnl.raw() < touch.first->pnl.raw(),
          "adding impact on top of the touch moves the result further down,"
          " never up -- a fill model that could improve a result would not be"
          " a cost model");

    // Unspecified refuses outright.
    Metronome s4{50};
    Ledger l4{Notional{10'000'000}};
    check(run(tape, quotes, kN, s4, l4, FillModel::Unspecified, sch, br, 0.0,
              Segment::Cash, Exchange::NSE).error()
          == BacktestError::NoFillModel,
          "and a backtest whose fill model was never chosen refuses to run --"
          " ordinal 0 is Unspecified because this is the single largest"
          " determinant of the answer and there is no safe default");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_limit_order_does_not_fill_because_the_price_touched_it()
{
    std::printf("\n2 a_limit_order_does_not_fill_because_the_price_touched"
                "_it\n");
    // THE CASE QUEUE POSITION DECIDES is touch-and-reverse: the price comes
    // down to the level, prints there a while, and leaves without clearing it.
    // (When the price trades THROUGH, the level cleared and queue position is
    // correctly irrelevant -- checked below. An earlier draft of this test used
    // a random tape that went straight through on tick 39, so both orders
    // filled together and the test measured the trade-through path while
    // claiming to measure the queue.)
    const Price limit{2'399'800};
    LimitOrder queued{};
    queued.side = Side::Buy;
    queued.limit = limit;
    queued.qty = Qty{75};
    queued.queue_ahead = Qty{600};      // 24 prints of 25 units ahead of us
    queued.active = true;

    LimitOrder front = queued;
    front.queue_ahead = Qty{0};         // front of queue, stated explicitly

    // Twelve prints AT the limit -- 300 units, a real visit to the level --
    // and then the price leaves upward without ever trading through.
    const int visit = 12;
    for (int i = 0; i < visit; ++i) {
        (void)apply_print_to_limit(queued, limit, Qty{25});
        (void)apply_print_to_limit(front, limit, Qty{25});
    }
    const bool naive_would_have = naive_limit_touched(queued, limit);

    std::printf("    a 75-unit buy limit with 600 units ahead of it, against"
                " %d prints of 25 units\n    AT the limit -- a visit to the"
                " level that reverses without clearing it:\n"
                "      naive 'the price touched it' rule       FILLED\n"
                "      FRONT of queue (0 ahead)                filled %ld of"
                " 75\n"
                "      600 units ahead                         filled %ld of"
                " 75, %ld still queued ahead\n",
                visit, static_cast<long>(front.filled.raw()),
                static_cast<long>(queued.filled.raw()),
                static_cast<long>(queued.queue_ahead.raw()));

    check(front.filled.raw() == 75 && !front.active,
          "at the front of the queue the order is done after three prints");
    check(queued.filled.raw() == 0 && queued.active,
          "with 600 units ahead of it, the SAME order at the SAME price over"
          " the SAME prints fills NOTHING -- 300 units traded and every one of"
          " them belonged to somebody who was there first");
    check(naive_would_have,
          "while the naive rule says filled, because the price touched the"
          " limit on the very first print");
    std::printf("    -> that is the difference between a backtest that books a"
                " passive fill and a\n       live account that watches the"
                " price come, sit, and leave. The naive rule\n       assumes"
                " permanent front-of-queue priority against every other"
                " participant --\n       the strongest possible claim, stated"
                " as an omission.\n");

    // And on a real tape, how often the naive rule would have claimed a fill.
    make_reverting_tape(0x111117A5, 5);
    LimitOrder probe{};
    probe.side = Side::Buy;
    probe.limit = limit;
    probe.qty = Qty{75};
    probe.active = true;
    int naive_touches = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        if (naive_limit_touched(probe, tape[i].last)) { ++naive_touches; }
    }
    std::printf("    over %zu ticks of a mean-reverting tape the naive rule"
                " claims a fill on %d of\n    them -- one order, filled %d"
                " times.\n", kN, naive_touches, naive_touches);
    check(naive_touches > 100,
          "the price reaches that level hundreds of times in a session, so the"
          " naive rule is not wrong occasionally -- it is wrong on every"
          " passive order a strategy ever rests");

    // Trading THROUGH the limit fills regardless of queue: the level cleared.
    LimitOrder deep{};
    deep.side = Side::Buy;
    deep.limit = Price{2'400'000};
    deep.qty = Qty{75};
    deep.queue_ahead = Qty{10'000};     // hopeless queue position
    deep.active = true;
    const auto got = apply_print_to_limit(deep, Price{2'399'000}, Qty{75});
    check(got.raw() == 75 && !deep.active,
          "but a print BELOW a buy limit fills it in full regardless of queue"
          " -- the book at that level cleared, which is what trading through"
          " means");

    // A print at the limit consumes queue first, and partial fills are real.
    LimitOrder partial{};
    partial.side = Side::Sell;
    partial.limit = Price{2'400'000};
    partial.qty = Qty{100};
    partial.queue_ahead = Qty{40};
    partial.active = true;
    const auto p1 = apply_print_to_limit(partial, Price{2'400'000}, Qty{60});
    check(p1.raw() == 20 && partial.active,
          "a 60-unit print against 40 units of queue fills 20 and leaves the"
          " order live -- partial fills are the normal case and the model"
          " produces them rather than rounding to all or nothing");
    const auto p2 = apply_print_to_limit(partial, Price{2'400'000}, Qty{500});
    check(p2.raw() == 80 && !partial.active,
          "and the next print completes it, taking only what is still wanted");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void conservation_is_checked_every_fill()
{
    std::printf("\n3 conservation_is_checked_every_fill\n");
    Ledger led{Notional{10'000'000}};
    Fill b{};
    b.side = Side::Buy;
    b.qty = Qty{75};
    b.price = Price{2'400'005};
    b.cost = Notional{2'347};
    check(led.apply(b).has_value(), "a buy applies");
    std::printf("    after one 75 x 24000.05 buy costing Rs 23.47:\n"
                "      cash Rs %12.2f   position %ld   residual %lld\n",
                rupees(led.cash().raw()), static_cast<long>(led.position().raw()),
                static_cast<long long>(led.residual()));
    check(led.residual() == 0 && led.position_residual() == 0,
          "and both conservation identities hold EXACTLY, in paise: opening"
          " minus bought plus sold minus costs equals cash, and bought minus"
          " sold equals position");

    Fill s = b;
    s.side = Side::Sell;
    s.price = Price{2'400'100};
    s.cost = Notional{2'352};
    check(led.apply(s).has_value(), "and the closing sell applies");
    check(led.position().raw() == 0 && led.residual() == 0,
          "the round trip returns to flat with the identity still exact");

    const Notional pnl = led.pnl(Price{2'400'100});
    std::printf("    round trip: bought 24000.05, sold 24001.00, costs"
                " Rs %.2f  ->  P&L Rs %.2f\n",
                rupees(b.cost.raw() + s.cost.raw()), rupees(pnl.raw()));
    check(pnl.raw() == 75 * (2'400'100 - 2'400'005) - 2'347 - 2'352,
          "and the P&L is the price difference times quantity minus both"
          " costs, to the paisa -- no rounding slack anywhere in the ledger");
    const double gross = static_cast<double>(75 * (2'400'100 - 2'400'005));
    std::printf("      gross Rs %.2f, charges Rs %.2f -- the bill took %.0f%%"
                " of the move\n",
                gross / 100.0, rupees(b.cost.raw() + s.cost.raw()),
                100.0 * static_cast<double>(b.cost.raw() + s.cost.raw())
                    / gross);
    check(pnl.raw() > 0 && static_cast<double>(pnl.raw()) < 0.5 * gross,
          "a 95-paise move on 75 units is Rs 71.25 gross and Rs 24.26 net:"
          " the charges take two thirds of it. Profitable, but only just, and"
          " a backtest that skipped the cost calculator would have booked"
          " three times the result");

    check(led.apply(Fill{}).error() == BacktestError::BadQuantity,
          "a zero-quantity fill is refused rather than silently applied as a"
          " no-op that still moves the cost");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_strategy_has_no_argument_that_could_carry_a_future_tick()
{
    std::printf("\n4 the_strategy_has_no_argument_that_could_carry_a_future"
                "_tick\n");
    make_tape(0xF07A, 5);

    // A strategy that records everything it is given, and then tries to find
    // anything about the future in it.
    struct Nosy {
        Timestamp latest{};
        std::size_t calls = 0;
        bool saw_future = false;
        Intent on_tick(const Tick& t, const Quote&, const Ledger&) noexcept {
            if (calls > 0 && t.exchange_ts <= latest) { saw_future = true; }
            latest = t.exchange_ts;
            ++calls;
            return Intent{};
        }
    } nosy;

    Ledger led{Notional{10'000'000}};
    const auto r = run(tape, quotes, kN, nosy, led, FillModel::Touch,
                       schedule(), broker(), 0.0, Segment::Cash, Exchange::NSE);
    check(r.has_value(), "the replay completes");
    check(nosy.calls == kN, "the strategy saw every tick, exactly once");
    check(!nosy.saw_future,
          "and strictly in time order -- each tick's exchange timestamp is"
          " later than the last");
    std::printf("    the strategy's on_tick receives (Tick, Quote, Ledger)."
                " There is no tape, no\n    index, no length and no peek: a"
                " future tick has no argument to arrive through.\n"
                "    %zu ticks delivered, %zu of them in order.\n",
                nosy.calls, nosy.calls);
    check(led.fills() == 0,
          "a strategy that never acts produces no fills, and no fills means no"
          " costs -- the engine does not charge for having looked");
    check(r->ticks == kN && r->pnl.raw() == 0,
          "so the run reports every tick and exactly zero P&L, which is the"
          " right answer for a strategy that did nothing");
}

} // namespace

int main()
{
    std::printf("altair backtest engine tests\n");
    the_fill_model_is_the_backtest();
    a_limit_order_does_not_fill_because_the_price_touched_it();
    conservation_is_checked_every_fill();
    the_strategy_has_no_argument_that_could_carry_a_future_tick();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
