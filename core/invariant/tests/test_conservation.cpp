// P0-09a acceptance tests for core/invariant/conservation.hpp.
// Plain main() (Catch2 blocked on vcpkg — see LEDGER blocker #7).
//
// Round numbers throughout so the arithmetic is checkable by eye:
// Price{2500000} is Rs 25,000.00 and Notional{12345} is Rs 123.45.

#include <invariant/conservation.hpp>

#include <chrono>
#include <cstdio>
#include <cstdint>

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

} // namespace

using namespace altair;

void test_ledger_sign_conventions()
{
    ConservationLedger L{Notional{100'000'000}};      // Rs 10,00,000.00

    // BUY 10 @ 25,000.00, cost Rs 123.45
    check(L.on_fill(Qty{10}, Price{2500000}, Notional{12345}).has_value(), "buy fills");
    check(L.position() == Qty{10}, "position long 10");
    check(L.fills_notional() == Notional{25'000'000}, "buy notional is positive");
    check(L.total_costs() == Notional{12345}, "cost recorded");
    check(L.cash() == Notional{100'000'000 - 25'000'000 - 12345},
          "buy: BOTH notional and cost subtract from cash");
    check(L.check().has_value(), "conservation holds after the buy");

    // SELL 10 @ 25,100.00, cost Rs 123.45
    check(L.on_fill(Qty{-10}, Price{2510000}, Notional{12345}).has_value(), "sell fills");
    check(L.position() == Qty{0} && L.is_flat(), "flat again");
    check(L.fills_notional() == Notional{25'000'000 - 25'100'000},
          "the sell's notional is NEGATIVE");
    check(L.total_costs() == Notional{24690}, "costs accumulated on both sides");
    check(L.check().has_value(), "conservation holds after the sell");

    // Gained Rs 1000.00 on the trade, paid Rs 246.90 in costs.
    check(L.cash_delta() == Notional{100'000 - 24'690},
          "realised = gross gain MINUS costs (== 75'310 paise)");
    // Had costs been CREDITED on the sell this would read 100'000, and every
    // short would look Rs 246.90 better than it was.

    check(L.on_fill(Qty{1}, Price{100}, Notional{-1}).error()
              == Breach::CashConservation,
          "a negative cost is rejected — that is a rebate, not a fill cost");
    check(L.fill_count() == 2, "and the rejected fill was not recorded");
}

void test_ledger_single_buy()
{
    ConservationLedger L{Notional{1'000'000}};
    check(L.initial_cash() == Notional{1'000'000}, "initial cash");
    check(L.cash() == Notional{1'000'000}, "cash starts at initial");
    check(L.position() == Qty{0} && L.is_flat(), "starts flat");
    check(L.fill_count() == 0, "no fills yet");
    check(L.check().has_value(), "an empty ledger balances");

    check(L.on_fill(Qty{5}, Price{1000}, Notional{50}).has_value(), "fill accepted");
    check(L.fill_count() == 1, "fill counted");
    check(L.fills_notional() == Notional{5000}, "notional == 1000 * 5");
    check(L.cash() == Notional{1'000'000 - 5000 - 50}, "cash reduced by both");
    check(L.cash_delta() == Notional{-5050}, "cash_delta is negative");
    check(L.check().has_value(), "conservation holds");
}

void test_ledger_buy_sell_roundtrip()
{
    ConservationLedger L{Notional{1'000'000}};
    (void)L.on_fill(Qty{100}, Price{500}, Notional{25});     // buy  100 @ 5.00
    (void)L.on_fill(Qty{-100}, Price{520}, Notional{26});    // sell 100 @ 5.20

    check(L.is_flat(), "round trip returns to flat");
    check(L.fills_notional() == Notional{50'000 - 52'000}, "net notional == -2000");
    check(L.total_costs() == Notional{51}, "costs total 51");
    check(L.cash_delta() == Notional{2000 - 51}, "winning round trip nets 1949");
    check(L.cash() == Notional{1'000'000 + 1949}, "cash reflects it");
    check(L.check().has_value(), "conservation holds");

    // A LOSING round trip: the sign must carry through.
    ConservationLedger M{Notional{1'000'000}};
    (void)M.on_fill(Qty{100}, Price{520}, Notional{26});
    (void)M.on_fill(Qty{-100}, Price{500}, Notional{25});
    check(M.cash_delta() == Notional{-2000 - 51}, "losing round trip nets -2051");
    check(M.check().has_value(), "conservation holds on a loss too");
}

void test_ledger_cash_adjustment()
{
    ConservationLedger L{Notional{1'000'000}};
    (void)L.on_fill(Qty{10}, Price{1000}, Notional{100});
    const Notional before = L.cash();

    check(L.on_cash_adjustment(Notional{500'000}).has_value(), "margin deposit");
    check(L.cash() == before + Notional{500'000}, "cash rose by the deposit");
    check(L.adjustments() == Notional{500'000}, "adjustment tracked separately");
    check(L.check().has_value(), "conservation STILL holds with an adjustment");

    check(L.on_cash_adjustment(Notional{-200'000}).has_value(), "withdrawal");
    check(L.adjustments() == Notional{300'000}, "adjustments net out");
    check(L.check().has_value(), "conservation holds after the withdrawal");

    check(L.total_costs() == Notional{100}, "an adjustment is not a cost");
    check(L.fills_notional() == Notional{10'000}, "an adjustment is not a fill");
}

void test_ledger_equity()
{
    ConservationLedger L{Notional{1'000'000}};
    check(L.equity(Price{999}).value() == Notional{1'000'000},
          "flat: equity == cash, whatever the mark");

    (void)L.on_fill(Qty{10}, Price{1000}, Notional{0});
    // cash = 1'000'000 - 10'000 = 990'000; position 10 @ mark 1000 = 10'000
    check(L.equity(Price{1000}).value() == Notional{1'000'000},
          "marked at cost: equity unchanged");
    check(L.equity(Price{1100}).value() == Notional{1'001'000},
          "mark up 100/unit: +1000 unrealised");
    check(L.equity(Price{900}).value() == Notional{999'000},
          "mark down 100/unit: -1000 unrealised");

    ConservationLedger H{Notional{0}};
    (void)H.on_fill(Qty{1'000'000'000}, Price{1}, Notional{0});
    check(H.equity(Price{Price::max().raw()}).error() == Breach::Overflow,
          "an overflowing mark is reported, not wrapped");
}

void test_ledger_conservation_over_many_fills()
{
    // A fixed LCG, not std::random: the sequence must be identical on every
    // box and every run, or a failure is not reproducible.
    std::uint64_t rng = 0x2026'08'29ull;
    auto next = [&rng]() noexcept -> std::uint64_t {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return rng >> 33;
    };

    ConservationLedger L{Notional{1'000'000'000}};

    // The test keeps its own independent running totals and compares at the end.
    std::int64_t exp_fills = 0, exp_costs = 0, exp_adj = 0;
    std::int64_t exp_cash = 1'000'000'000;
    std::uint64_t issued = 0;
    bool every_check_passed = true;

    for (int i = 0; i < 10'000; ++i) {
        if ((next() % 20) == 0) {
            const std::int64_t amt =
                static_cast<std::int64_t>(next() % 200'001) - 100'000;
            if (L.on_cash_adjustment(Notional{amt}).has_value()) {
                exp_adj += amt;
                exp_cash += amt;
            }
        } else {
            std::int64_t q = static_cast<std::int64_t>(next() % 101) - 50;
            if (q == 0) { q = 1; }
            const std::int64_t px = 100 + static_cast<std::int64_t>(next() % 9901);
            const std::int64_t cost = static_cast<std::int64_t>(next() % 501);

            if (L.on_fill(Qty{q}, Price{px}, Notional{cost}).has_value()) {
                ++issued;
                exp_fills += px * q;
                exp_costs += cost;
                exp_cash -= (px * q + cost);
            }
        }
        // The identity must hold at EVERY step, not just at the end.
        if (!L.check().has_value()) {
            every_check_passed = false;
        }
    }

    check(every_check_passed, "conservation held after every one of 10'000 operations");
    check(L.fill_count() == issued, "fill count matches what was actually accepted");
    check(!L.is_breached(), "no breach latched");

    // Recompute independently from the totals the test kept itself.
    check(L.fills_notional() == Notional{exp_fills}, "fills match an independent sum");
    check(L.total_costs() == Notional{exp_costs}, "costs match an independent sum");
    check(L.adjustments() == Notional{exp_adj}, "adjustments match");
    check(L.cash() == Notional{exp_cash}, "cash matches an independent sum");

    std::printf("        %llu fills, position %lld, cash_delta %lld paise\n",
                static_cast<unsigned long long>(L.fill_count()),
                static_cast<long long>(L.position().raw()),
                static_cast<long long>(L.cash_delta().raw()));
}

void test_ledger_overflow_rejects_and_mutates_nothing()
{
    ConservationLedger L{Notional{0}};
    check(L.on_fill(Qty{Qty::max().raw()}, Price{2}, Notional{0}).error()
              == Breach::Overflow,
          "a fill whose notional overflows is rejected");
    check(L.fill_count() == 0, "NOTHING was recorded");
    check(L.position() == Qty{0}, "position untouched");
    check(L.cash() == Notional{0}, "cash untouched");
    check(L.fills_notional() == Notional{0}, "fills untouched");
    check(!L.is_breached(), "a rejected fill is not itself a breach");
    check(L.check().has_value(), "and the books still balance");

    // Overflow on the cost ACCUMULATOR rather than the notional.
    // Note the headroom: a cost of exactly Notional::max() would make
    // notional + cost unrepresentable and be refused on the FIRST fill, which
    // would test the wrong guard. Leave room for the notional.
    const Notional huge{Notional::max().raw() - 1000};
    ConservationLedger M{Notional{0}};
    check(M.on_fill(Qty{1}, Price{1}, huge).has_value(),
          "one enormous cost fits, with room for the notional");
    check(M.on_fill(Qty{1}, Price{1}, huge).error() == Breach::Overflow,
          "a second would overflow the cost accumulator");
    check(M.fill_count() == 1, "the second fill was rejected whole");
    check(M.check().has_value(), "conservation still holds");

    // And the guard that the first case would have tripped: an outflow that
    // cannot be represented even though each accumulator individually could.
    ConservationLedger N{Notional{0}};
    check(N.on_fill(Qty{1}, Price{1}, Notional{Notional::max().raw()}).error()
              == Breach::Overflow,
          "an unrepresentable notional+cost outflow is refused");
    check(N.fill_count() == 0, "and nothing was recorded");
}

void test_ledger_breach_latches_and_blocks()
{
    ConservationLedger L{Notional{1'000'000}};
    (void)L.on_fill(Qty{1}, Price{100}, Notional{5});
    check(!L.is_breached(), "not breached");
    check(L.breach_count() == 0, "no breaches yet");

    L.trip(Breach::CashConservation);
    check(L.is_breached(), "trip latches");
    check(L.latched_breach() == Breach::CashConservation, "the breach is recorded");
    check(L.breach_count() == 1, "breach counted");

    // While latched, EVERYTHING is refused — the kill switch.
    check(L.on_fill(Qty{1}, Price{100}, Notional{5}).error() == Breach::Latched,
          "fills are refused while latched");
    check(L.on_cash_adjustment(Notional{1}).error() == Breach::Latched,
          "adjustments are refused while latched");
    check(L.check().error() == Breach::Latched, "check reports the latch");
    check(L.fill_count() == 1, "nothing got through");

    // A second trip does NOT overwrite the first — the FIRST is the diagnosis.
    L.trip(Breach::Overflow);
    check(L.latched_breach() == Breach::CashConservation,
          "the FIRST breach survives a second trip");
    check(L.breach_count() == 2, "but the count still moves");

    L.clear_breach();
    check(!L.is_breached(), "clear_breach lifts the latch");
    check(L.breach_count() == 2, "and leaves the audit trail intact");
    check(L.on_fill(Qty{1}, Price{100}, Notional{5}).has_value(), "trading resumes");
    check(L.check().has_value(), "balances were never touched by any of it");
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");
    constexpr int kOps = 1'000'000;

    ConservationLedger L{Notional{1'000'000'000'000}};
    std::uint64_t sink = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        // Alternate sides so the position stays bounded.
        const std::int64_t q = (i % 2 == 0) ? 1 : -1;
        sink += L.on_fill(Qty{q}, Price{1000}, Notional{1}).has_value() ? 1u : 0u;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns0 = std::chrono::duration<double, std::nano>(t1 - t0).count()
                     / static_cast<double>(kOps);
    std::printf("  on_fill                       %6.2f ns/call (budget < 20)  %s\n",
                ns0, ns0 < 20.0 ? "OK" : "OVER");

    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        sink += L.check_and_trip().has_value() ? 1u : 0u;
    }
    const auto t3 = std::chrono::steady_clock::now();
    const double ns1 = std::chrono::duration<double, std::nano>(t3 - t2).count()
                     / static_cast<double>(kOps);
    std::printf("  check_and_trip                %6.2f ns/call (budget < 20)  %s\n",
                ns1, ns1 < 20.0 ? "OK" : "OVER");
    std::printf("    (ROADMAP §11 budgets all risk checks at 3 us; this runs inside it)\n");

    if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf("  (unreachable)\n"); }
}

} // namespace

int main()
{
    std::printf("altair core/invariant conservation tests\n");
    test_ledger_sign_conventions();
    test_ledger_single_buy();
    test_ledger_buy_sell_roundtrip();
    test_ledger_cash_adjustment();
    test_ledger_equity();
    test_ledger_conservation_over_many_fills();
    test_ledger_overflow_rejects_and_mutates_nothing();
    test_ledger_breach_latches_and_blocks();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
