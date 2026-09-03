// risk/tests/test_charges_toml.cpp -- P3-09b.
//
// Runs against the REAL config/charges.toml, not a fixture.
//
// A fixture would test the parser against text written to make the parser
// pass. The defect this card corrects is that the shipped file was never read
// by anything, so the one assertion that matters is that THIS file, as it sits
// on disk, produces the schedules the cost calculator needs.
//
// The old hand-mirrored constants in test_cost.cpp stay where they are: they
// now serve as an independent second opinion, and one of these tests compares
// the two. If someone edits charges.toml and not the mirror, that comparison
// is what says so -- which is exactly the drift that went unnoticed until now.

#include <risk/charges_toml.hpp>
#include <risk/cost.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// UTC nanoseconds for a date at 09:15, a time inside the trading day.
constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;
constexpr std::int64_t at(std::int64_t days, int h, int m) {
    return days * kNsPerDay + (h * 3600LL + m * 60LL) * 1'000'000'000LL;
}
/// Days from 1970-01-01, same algorithm as the loader.
constexpr std::int64_t civil(std::int64_t y, unsigned mo, unsigned d) {
    y -= mo <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

} // namespace

int main() {
    std::printf("P3-09b charges.toml loader\n");

    std::vector<altair::ChargeSchedule> sch;
    const auto rep = altair::load_charges_file(ALTAIR_CHARGES_TOML, sch);
    if (!rep) {
        std::printf("  FAIL: %s (%s)\n",
                    altair::charges_error_text(rep.error()),
                    ALTAIR_CHARGES_TOML);
        return 1;
    }
    check(true, "config/charges.toml parsed");
    std::printf("    %zu schedules, %zu segment tables, verified=%s\n",
                rep->schedules, rep->segment_tables,
                rep->verified ? "yes" : "NO");

    check(rep->schedules >= 2, "at least two effective-dated schedules");
    check(rep->segment_tables >= 12, "segment tables were actually read");

    // D7. The file says UNVERIFIED today, and that must propagate rather than
    // being something a caller can override.
    check(!rep->verified,
          "last_verified is UNVERIFIED, so verified=false — if this FAILS, "
          "someone set the date and that is good news");
    bool all_unverified = true;
    for (const auto& s : sch) {
        if (s.verified != rep->verified) { all_unverified = false; }
    }
    check(all_unverified, "every schedule carries the file's verification state");

    // ---- THE BOUNDARY THIS CARD EXISTS AROUND ----------------------------
    //
    // CLAUDE.md: "STT rose on 2026-04-01 (futures 0.02 -> 0.05%, options 0.10
    // -> 0.15% sell-side premium). Every pre-April backtest is optimistic
    // until re-run." That statement is only actionable if the backtester can
    // reach BOTH schedules, and the failure mode is silent: parse valid_to as
    // midnight and every trade on 31 March gets no schedule at all.
    const auto* mar31 = altair::schedule_for(
        sch.data(), sch.size(), altair::Timestamp{at(civil(2026, 3, 31), 9, 15)});
    const auto* apr01 = altair::schedule_for(
        sch.data(), sch.size(), altair::Timestamp{at(civil(2026, 4, 1), 9, 15)});
    const auto* mar31_late = altair::schedule_for(
        sch.data(), sch.size(), altair::Timestamp{at(civil(2026, 3, 31), 23, 59)});

    check(mar31 != nullptr, "2026-03-31 09:15 has a schedule");
    check(mar31_late != nullptr,
          "2026-03-31 23:59 has one too — valid_to covers the WHOLE day");
    check(apr01 != nullptr, "2026-04-01 09:15 has a schedule");
    check(mar31 != apr01, "and they are DIFFERENT schedules");

    if (mar31 != nullptr && apr01 != nullptr && mar31 != apr01) {
        std::printf("\n  the 2026-04-01 STT rise, read from the file:\n");
        std::printf("    equity_futures stt   %lld -> %lld nano\n",
                    static_cast<long long>(mar31->equity_futures.stt),
                    static_cast<long long>(apr01->equity_futures.stt));
        std::printf("    equity_options stt   %lld -> %lld nano\n",
                    static_cast<long long>(mar31->equity_options.stt),
                    static_cast<long long>(apr01->equity_options.stt));
        check(apr01->equity_futures.stt > mar31->equity_futures.stt,
              "futures STT is HIGHER after 2026-04-01");
        check(apr01->equity_options.stt > mar31->equity_options.stt,
              "options STT is HIGHER after 2026-04-01");
    }

    // ---- premium-vs-notional survived the round trip ---------------------
    //
    // Gate 7's headline trap. If this came back Notional, every option would
    // be charged on strike rather than premium -- a factor of 500 on a NIFTY
    // 25000 call at Rs 50, in the direction that kills every options strategy
    // before it is written.
    if (apr01 != nullptr) {
        check(apr01->equity_options.basis == altair::TurnoverBasis::Premium,
              "equity_options turnover_basis is PREMIUM");
        check(apr01->equity_futures.basis == altair::TurnoverBasis::Notional,
              "equity_futures turnover_basis is NOTIONAL");
        check(apr01->equity_options.stt_side == altair::ChargeSide::Sell,
              "options STT is SELL side only");
        check(apr01->equity_delivery.stt_side == altair::ChargeSide::Both,
              "delivery STT is BOTH sides");
    }

    // ---- and it prices a real trade --------------------------------------
    if (apr01 != nullptr) {
        altair::Trade t{};
        t.segment = altair::Segment::Opt;
        t.exchange = altair::Exchange::NSE;
        t.side = altair::Side::Sell;
        t.qty = altair::Qty{75};
        t.price = altair::Price{5'000};        // Rs 50.00 premium
        t.trade_ts = altair::Timestamp{at(civil(2026, 6, 1), 9, 15)};

        altair::BrokerageRule br{};
        br.flat_per_order = altair::Notional{2'000};   // Rs 20
        br.pct = 0;
        br.take_lower = true;

        const auto cb = altair::compute_cost(t, *apr01, br);
        check(cb.has_value(), "a real option sell priced from the loaded file");
        if (cb) {
            std::printf("\n  75 x Rs 50 NIFTY option SELL, priced from "
                        "charges.toml:\n");
            std::printf("    turnover     %10lld paise\n",
                        static_cast<long long>(cb->turnover.raw()));
            std::printf("    brokerage    %10lld\n",
                        static_cast<long long>(cb->brokerage.raw()));
            std::printf("    STT          %10lld\n",
                        static_cast<long long>(cb->stt.raw()));
            std::printf("    exchange     %10lld\n",
                        static_cast<long long>(cb->exchange_txn.raw()));
            std::printf("    SEBI         %10lld\n",
                        static_cast<long long>(cb->sebi.raw()));
            std::printf("    stamp        %10lld\n",
                        static_cast<long long>(cb->stamp.raw()));
            std::printf("    IPFT         %10lld\n",
                        static_cast<long long>(cb->ipft.raw()));
            std::printf("    GST          %10lld\n",
                        static_cast<long long>(cb->gst.raw()));
            std::printf("    TOTAL        %10lld paise  = Rs %.2f\n",
                        static_cast<long long>(cb->total.raw()),
                        static_cast<double>(cb->total.raw()) / 100.0);
            // Turnover is PREMIUM: 75 x Rs 50 = Rs 3,750 = 3,75,000 paise.
            // Notional would be 75 x strike, which is not even in the Trade.
            check(cb->turnover.raw() == 375'000,
                  "turnover is premium (Rs 3,750), not notional");
            check(!cb->schedule_verified,
                  "the breakdown says its schedule is UNVERIFIED");
            // Sum of parts equals the total, exactly, in integer paise.
            const std::int64_t parts =
                cb->brokerage.raw() + cb->stt.raw() + cb->exchange_txn.raw()
                + cb->sebi.raw() + cb->stamp.raw() + cb->ipft.raw()
                + cb->gst.raw() + cb->dp.raw();
            check(parts == cb->total.raw(),
                  "itemised components sum EXACTLY to the total");
        }
    }

    // ---- refusals --------------------------------------------------------
    //
    // The safety argument of the whole file: a bad rate must not become zero.
    std::vector<altair::ChargeSchedule> junk;
    const char* bad_rate =
        "last_verified=\"UNVERIFIED\"\n[[schedule]]\n"
        "valid_from=\"2020-01-01\"\nvalid_to=\"2030-01-01\"\n"
        "[schedule.equity_futures]\nstt_rate=\"not a number\"\n";
    const auto r1 = altair::load_charges(bad_rate,
                                         std::char_traits<char>::length(bad_rate),
                                         junk);
    check(!r1 && r1.error() == altair::ChargesError::BadRate,
          "an unparseable rate is REFUSED, not treated as zero");

    const char* bad_side =
        "[[schedule]]\nvalid_from=\"2020-01-01\"\nvalid_to=\"2030-01-01\"\n"
        "[schedule.equity_futures]\nstt_rate=0.0002\nstt_side=\"sometimes\"\n";
    const auto r2 = altair::load_charges(bad_side,
                                         std::char_traits<char>::length(bad_side),
                                         junk);
    check(!r2 && r2.error() == altair::ChargesError::BadEnum,
          "an unrecognised side is REFUSED");

    const char* overlap =
        "[[schedule]]\nvalid_from=\"2020-01-01\"\nvalid_to=\"2026-12-31\"\n"
        "[[schedule]]\nvalid_from=\"2026-01-01\"\nvalid_to=\"2030-12-31\"\n";
    const auto r3 = altair::load_charges(overlap,
                                         std::char_traits<char>::length(overlap),
                                         junk);
    check(!r3 && r3.error() == altair::ChargesError::OverlappingSchedules,
          "two schedules covering one instant are REFUSED — file order must "
          "not silently pick a backtest's rates");

    const char* no_dates =
        "[[schedule]]\n[schedule.equity_futures]\nstt_rate=0.0002\n";
    const auto r4 = altair::load_charges(no_dates,
                                         std::char_traits<char>::length(no_dates),
                                         junk);
    check(!r4 && r4.error() == altair::ChargesError::BadDateRange,
          "an undated schedule is REFUSED");

    // An ABSENT segment table is not an error -- it is "this schedule has no
    // such line", and compute_cost turns that into UnknownSegment rather than
    // a free trade. Distinct from a table that is present and malformed.
    const char* sparse =
        "[[schedule]]\nvalid_from=\"2020-01-01\"\nvalid_to=\"2030-01-01\"\n"
        "[schedule.equity_futures]\nstt_rate=0.0002\nstt_side=\"sell\"\n";
    std::vector<altair::ChargeSchedule> one;
    const auto r5 = altair::load_charges(sparse,
                                         std::char_traits<char>::length(sparse),
                                         one);
    check(r5.has_value() && one.size() == 1,
          "a schedule with only some segments LOADS");
    if (r5 && one.size() == 1) {
        check(one[0].equity_futures.present && !one[0].equity_options.present,
              "an ABSENT segment is present=false, not a zero-rate segment");
        altair::Trade t{};
        t.segment = altair::Segment::Opt;
        t.qty = altair::Qty{75};
        t.price = altair::Price{5'000};
        t.trade_ts = altair::Timestamp{at(civil(2025, 6, 1), 9, 15)};
        altair::BrokerageRule br{};
        const auto cb = altair::compute_cost(t, one[0], br);
        check(!cb && cb.error() == altair::CostError::UnknownSegment,
              "and a trade in that segment is REFUSED, not priced at zero");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
