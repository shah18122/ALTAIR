// oms/tests/test_execution.cpp -- P13-01..06.
//
// The conservation law of execution: SUM OF CHILDREN EQUALS THE PARENT,
// exactly, in integer quantity. It is violated the way conservation of cash
// is -- not by one big error but by rounding, repeatedly, in the same
// direction -- so every schedule here is checked against its parent rather
// than trusted.

#include <oms/execution.hpp>
#include <oms/shortfall.hpp>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// 09:15 IST as a UTC instant, and 15:30 the same day.
constexpr altair::Timestamp kOpen{1'788'407'100'000'000'000LL};
const altair::Timestamp kClose = kOpen + altair::duration::minutes(375);

} // namespace

int main() {
    using altair::AcParams;
    using altair::ExecError;
    using altair::Notional;
    using altair::Price;
    using altair::Qty;
    using altair::almgren_chriss;
    using altair::attribute;
    using altair::attribution_sums;
    using altair::pov_step;
    using altair::twap_schedule;
    using altair::vwap_schedule;

    std::printf("P13 execution algorithms\n");

    // ---- 1. THE ROUNDING THAT LOSES SHARES --------------------------------
    //
    // 1000 over 7 is 142.857. Seven lots of 142 is 994 and the parent is six
    // short with every child reporting success.
    {
        const auto s = twap_schedule(Qty{1000}, kOpen, kClose, 7);
        check(s.has_value(), "a TWAP that does not divide evenly still builds");
        if (s) {
            std::printf("    1000 over 7 slices:");
            for (std::size_t i = 0; i < s->size(); ++i) {
                std::printf(" %lld", static_cast<long long>((*s)[i].qty.raw()));
            }
            std::printf("  = %lld\n",
                        static_cast<long long>(s->total().raw()));
            check(s->total().raw() == 1000,
                  "and the children sum to the parent EXACTLY -- naive "
                  "integer division would have sent 994 and reported success "
                  "on all seven");

            std::int64_t lo = (*s)[0].qty.raw(), hi = lo;
            for (std::size_t i = 1; i < s->size(); ++i) {
                const std::int64_t q = (*s)[i].qty.raw();
                if (q < lo) { lo = q; }
                if (q > hi) { hi = q; }
            }
            check(hi - lo <= 1,
                  "the remainder is spread one unit at a time rather than "
                  "dumped on the last slice, so no child is materially larger "
                  "than the others -- the big one would be both the most "
                  "market-moving and the most likely to hit a POV cap");
        }
    }

    // ---- 2. THE LAST CHILD LANDS BEFORE THE DEADLINE ----------------------
    {
        const auto s = twap_schedule(Qty{600}, kOpen, kClose, 6);
        if (s) {
            check((*s)[0].at.ns_since_epoch() == kOpen.ns_since_epoch(),
                  "the first slice works at the window open");
            check((*s)[s->size() - 1].at < kClose,
                  "and the last works strictly BEFORE the close -- slicing at "
                  "interval midpoints or ends puts the final child past the "
                  "deadline the parent was given");
        }
    }

    // ---- 3. AN INDEX HAS NO VOLUME, AND THAT IS NOT A FLAT CURVE ----------
    {
        const double curve[6] = {30.0, 12.0, 8.0, 8.0, 14.0, 28.0};  // U-shape
        const auto idx = vwap_schedule(Qty{1000}, kOpen, kClose, curve, 6,
                                       /*reports_volume=*/false);
        check(!idx && idx.error() == ExecError::NoVolume,
              "a VWAP for an INDEX is refused, not degraded to TWAP -- NIFTY "
              "50 does not trade, and quietly falling back would report a "
              "benchmark nobody can compute as met");

        const auto s = vwap_schedule(Qty{1000}, kOpen, kClose, curve, 6, true);
        check(s.has_value(), "and for a real instrument it schedules");
        if (s) {
            std::printf("    U-shaped curve 30/12/8/8/14/28 ->");
            for (std::size_t i = 0; i < s->size(); ++i) {
                std::printf(" %lld", static_cast<long long>((*s)[i].qty.raw()));
            }
            std::printf("  = %lld\n",
                        static_cast<long long>(s->total().raw()));
            check(s->total().raw() == 1000,
                  "largest-remainder apportionment keeps the parent exact "
                  "where rounding each share independently would not");
            check((*s)[0].qty.raw() > (*s)[2].qty.raw(),
                  "and it front-loads into the open, where the volume is -- "
                  "which is the entire difference from TWAP");
        }
    }

    // ---- 4. A CURVE THAT CANNOT BE NORMALISED -----------------------------
    {
        const double neg[3] = {1.0, -1.0, 1.0};
        check(!vwap_schedule(Qty{100}, kOpen, kClose, neg, 3, true).has_value(),
              "a negative volume bucket is refused rather than normalised "
              "into a negative child order");
        const double zero[3] = {0.0, 0.0, 0.0};
        check(!vwap_schedule(Qty{100}, kOpen, kClose, zero, 3, true).has_value(),
              "and an all-zero curve is refused rather than dividing by zero");
    }

    // ---- 5. POV IS FEEDBACK, AND IT SAYS WHEN IT IS BOUND -----------------
    {
        const auto easy = pov_step(Qty{100}, 10000, 0.10);
        check(easy && easy->send.raw() == 100 && !easy->capped,
              "with ample volume POV sends the remainder and is not capped");

        const auto tight = pov_step(Qty{1000}, 2000, 0.10);
        check(tight && tight->send.raw() == 200 && tight->capped,
              "in thin volume it sends 10% of what traded and REPORTS being "
              "capped -- a POV permanently at its cap is not participating at "
              "the requested rate, it is falling behind, and the caller has "
              "to know before the close rather than after");

        const auto dry = pov_step(Qty{1000}, 5, 0.10);
        check(dry && dry->send.raw() == 0 && dry->capped,
              "and when the rate rounds to nothing it sends NOTHING rather "
              "than one unit -- one lot into a five-lot bucket is "
              "participating at 20%, not 10%");
    }

    // ---- 6. THE RISK-NEUTRAL LIMIT OF ALMGREN-CHRISS IS TWAP --------------
    //
    // The sanity check that proves the closed form was implemented and not
    // merely typed: as risk aversion goes to zero there is no reason to hurry,
    // and the optimal trajectory must become the uniform one.
    {
        AcParams p;
        p.sigma = 0.20;
        p.eta = 1.0e-6;
        p.lambda = 1.0e-12;                 // very nearly risk-neutral
        const auto t = almgren_chriss(Qty{1000}, 1.0 / 365.0, p, 10);
        check(t.has_value(), "the trajectory solves");
        if (t) {
            std::int64_t lo = t->trade(1), hi = lo;
            for (std::size_t i = 2; i <= t->steps(); ++i) {
                const std::int64_t q = t->trade(i);
                if (q < lo) { lo = q; }
                if (q > hi) { hi = q; }
            }
            std::printf("    lambda=1e-12: per-step trades range %lld..%lld"
                        "  (kappa*T %.2e)\n",
                        static_cast<long long>(lo),
                        static_cast<long long>(hi), t->kappa_t());
            check(hi - lo <= 1,
                  "at zero risk aversion the optimal trajectory IS TWAP -- a "
                  "risk-neutral trader has no reason to hurry, and this is "
                  "the limit that shows the sinh was solved and not merely "
                  "typed");
            check(t->total_traded() == 1000,
                  "and it still sums to the parent after rounding");
        }
    }

    // ---- 7. RISK AVERSION FRONT-LOADS -------------------------------------
    {
        AcParams p;
        p.sigma = 0.20;
        p.eta = 1.0e-6;
        // lambda chosen so kappa*T lands ABOVE 1, which is what makes a
        // trajectory risk-averse. The first draft used 1e-3, read kappa =
        // 6.3, called it strongly risk-averse and got a UNIFORM schedule --
        // because over a one-day horizon kappa*T was 0.017 and the sinh was
        // still linear. kappa has units of 1/time and means nothing alone.
        p.lambda = 10.0;
        const auto t = almgren_chriss(Qty{1000}, 1.0 / 365.0, p, 10);
        if (t) {
            std::printf("    lambda=10   : first step %lld, last step %lld"
                        "  (kappa*T %.2f)\n",
                        static_cast<long long>(t->trade(1)),
                        static_cast<long long>(t->trade(t->steps())),
                        t->kappa_t());
            check(t->kappa_t() > 1.0,
                  "kappa*T is above 1, so this really is the risk-averse "
                  "regime -- the DIMENSIONLESS group, not kappa, is what "
                  "says which side of the trade-off a trajectory sits on");
            check(t->trade(1) > t->trade(t->steps()),
                  "a risk-averse trajectory front-loads: get out now and pay "
                  "impact for it, which is the trade-off the parameter exists "
                  "to express");
            check(t->total_traded() == 1000,
                  "and it too sums to the parent");
            bool monotone = true;
            for (std::size_t i = 1; i <= t->steps(); ++i) {
                if (t->trade(i) < 0) { monotone = false; }
            }
            check(monotone,
                  "no step trades a NEGATIVE quantity, which after rounding "
                  "would read as buying during a sell programme");
        }
    }

    check(!almgren_chriss(Qty{1000}, 1.0, AcParams{}, 10).has_value(),
          "and risk aversion has NO default: an unset AcParams is refused, "
          "because lambda is a statement about how much Smit will pay to "
          "reduce variance and no constant can make it for him");

    // ---- 8. THE ATTRIBUTION MUST SUM --------------------------------------
    //
    // Four components that each look plausible and do not add up is the exact
    // failure an attribution exists to prevent.
    {
        // Buy 100 lots. Decided at 24,000.00, market had moved to 24,010.00 by
        // the time we started, filled 80 at 24,025.00, the rest never done and
        // the close was 24,050.00. Fees Rs 120.00.
        const auto s = attribute(+1, Price{2'400'000}, Price{2'401'000},
                                 Price{2'402'500}, Qty{100}, Qty{80},
                                 Price{2'405'000}, Notional{12'000});
        std::printf("    delay %lld  execution %lld  fees %lld"
                    "  opportunity %lld  = %lld paise\n",
                    static_cast<long long>(s.delay_paise),
                    static_cast<long long>(s.execution_paise),
                    static_cast<long long>(s.fees_paise),
                    static_cast<long long>(s.opportunity_paise),
                    static_cast<long long>(s.total()));
        check(attribution_sums(+1, Price{2'400'000}, Price{2'402'500},
                               Qty{100}, Qty{80}, Price{2'405'000},
                               Notional{12'000}, s),
              "the four parts sum to a total computed INDEPENDENTLY from the "
              "decision price, the realised average and the close -- if they "
              "did not, one component would be wrong and the other three "
              "would look fine");
        check(s.opportunity_paise > 0,
              "and the unfilled 20 lots cost money because the market rose "
              "away from us -- the component everyone forgets, without which "
              "an algorithm that fills 60%% reports a wonderful average price "
              "for having done less");
    }

    // ---- 9. THE SIGN THAT MAKES EVERY SHORT LOOK PROFITABLE ---------------
    {
        // Identical prices, opposite side. A rising market costs a buyer and
        // pays a seller, and a shortfall that gets this wrong reports both as
        // gains.
        const auto buy = attribute(+1, Price{2'400'000}, Price{2'401'000},
                                   Price{2'402'500}, Qty{100}, Qty{100},
                                   Price{2'405'000}, Notional{0});
        const auto sell = attribute(-1, Price{2'400'000}, Price{2'401'000},
                                    Price{2'402'500}, Qty{100}, Qty{100},
                                    Price{2'405'000}, Notional{0});
        check(buy.execution_paise == -sell.execution_paise
                  && buy.execution_paise > 0,
              "a rising market is a COST to the buyer and a GAIN to the "
              "seller, exactly equal and opposite -- the sign error here is "
              "the one that makes every short look profitable");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
