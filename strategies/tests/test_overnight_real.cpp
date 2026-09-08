// P22-02 -- the overnight gap on REAL NIFTY.
//
// P21-03 found the session hours were a net drag and the index's whole move
// arrived overnight. P22-01 built the strategy that would exploit that, on
// constructed data where every answer was known. This runs it on the eleven
// years of five-minute NIFTY in `dataset/`.
//
// WHAT P21-03 MEASURED, AND WHY THIS NUMBER IS DIFFERENT.
//
// P21-03's "overnight" bucket ran from the last five-minute CLOSE of one
// session to the first five-minute CLOSE of the next -- 15:30 to 09:20 -- and
// was right to, because the strategies in that file trade on bar closes and
// that is the boundary they see.
//
// A trader acting on the gap does not see that boundary. They see the CLOSE
// and the OPEN, 15:30 to 09:15, and the five minutes between 09:15 and 09:20
// belong to the session. Those five minutes are not incidental: they are where
// the gap starts filling, and moving the boundary changes the answer by nearly
// forty per cent. Both are computed here, side by side, and the difference
// between them IS the execution risk in this strategy rather than a footnote
// about it.
//
// THE COST NUMBER IS NOT A LITERAL AND IS NOT ASSUMED.
//
// Rule 1 forbids a rate in the source. So nothing here asserts that the
// strategy is or is not profitable at a particular cost. It computes the
// BREAK-EVEN round trip -- the cost at which the strategy stops paying -- and
// prints a sweep, and the reader brings the real rate from
// `config/charges.toml`. A break-even is a property of the data; a verdict
// would be a property of a constant somebody typed.
//
// AND TWO THINGS A FULL-SAMPLE MEAN HIDES, BOTH REPORTED BELOW.
//
// The payoff is LEFT-SKEWED -- the median night pays more than the mean night,
// so this is a premium-collecting strategy wearing occasional shocks -- and the
// effect DECAYS: the most recent years are a different number from the eleven-
// year average, and it is the recent number a decision today rests on.
//
// No check description here may contain the substring FAIL.

#include <strategies/overnight.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef ALTAIR_DATASET_DIR
#  define ALTAIR_DATASET_DIR "dataset"
#endif

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

/// Sessions from the monthly-partitioned five-minute files.
///
/// The session's OPEN is the open of its first bar -- the pre-open auction
/// print -- not the close of that bar. Those differ by the most volatile five
/// minutes of the day, which is the entire subject of this file, so reading
/// the wrong column here would silently answer a different question.
std::vector<altair::SessionBar> load_sessions()
{
    const std::filesystem::path dir =
        std::filesystem::path(ALTAIR_DATASET_DIR) / "spot" / "nifty" / "5m";
    std::vector<altair::SessionBar> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) { return out; }

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());

    std::string current;
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);                      // header
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            std::string day;
            double o = 0.0, c = 0.0;
            while (std::getline(ss, cell, ',')) {
                if (col == 0 && cell.size() >= 10) { day = cell.substr(0, 10); }
                if (col == 1 && !cell.empty()) { o = std::atof(cell.c_str()); }
                if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
                ++col;
            }
            if (day.empty() || !(o > 0.0) || !(c > 0.0)) { continue; }
            if (day != current) {
                current = day;
                altair::SessionBar b;
                b.open = o;                  // the auction print
                b.first_bar_close = c;       // 09:20
                b.close = c;                 // updated as the session runs
                b.day = day;
                out.push_back(b);
            } else {
                out.back().close = c;
            }
        }
    }
    return out;
}

void mean_sd_t(const std::vector<double>& v, double& m, double& sd, double& t)
{
    m = sd = t = 0.0;
    if (v.size() < 2) { return; }
    double s = 0.0;
    for (const double x : v) { s += x; }
    const double n = static_cast<double>(v.size());
    m = s / n;
    double s2 = 0.0;
    for (const double x : v) { s2 += (x - m) * (x - m); }
    sd = std::sqrt(s2 / (n - 1.0));
    const double se = sd / std::sqrt(n);
    t = se > 0.0 ? m / se : 0.0;
}

} // namespace

using namespace altair;

int main()
{
    std::printf("P22-02 -- the overnight gap on real NIFTY\n");
    std::printf("Dataset: %s\n", ALTAIR_DATASET_DIR);

    // -----------------------------------------------------------------------
    // 1. Load, and check the session boundaries are sane.
    // -----------------------------------------------------------------------
    std::printf("\n[1] sessions from the five-minute partition\n");
    const auto bars = load_sessions();
    std::printf("        %zu sessions\n", bars.size());
    check(bars.size() > 2000, "at least 2000 sessions loaded");
    if (bars.size() <= 2000) {
        std::printf("\nFAILED -- dataset incomplete\n");
        return 1;
    }
    std::size_t open_is_first_close = 0;
    for (const SessionBar& b : bars) {
        if (b.open == b.first_bar_close) { ++open_is_first_close; }
    }
    std::printf("        sessions whose open equals their 09:20 close: %zu\n",
                open_is_first_close);
    // If these were mostly equal, the open column would be a copy of the close
    // column and every number below would be measuring one boundary twice.
    check(open_is_first_close < bars.size() / 10,
          "the open and the first bar's close are genuinely different prices");

    // -----------------------------------------------------------------------
    // 2. The decomposition, and the identity it must satisfy.
    // -----------------------------------------------------------------------
    std::printf("\n[2] where the day's return actually sits\n");
    const auto r = session_returns(bars);
    check(r.has_value(), "the decomposition was produced");
    if (!r) { return 1; }

    double worst = 0.0;
    for (std::size_t i = 0; i < r->overnight.size(); ++i) {
        worst = std::max(worst, std::fabs(r->overnight[i] + r->session[i]
                                          - r->close_to_close[i]));
    }
    check(worst < 1e-9,
          "overnight plus session reconstructs close-to-close exactly");

    struct Row { const char* name; const std::vector<double>* v; };
    const Row rows[] = {
        {"overnight   close -> open ", &r->overnight},
        {"proxy       close -> 09:20", &r->first_bar},
        {"session     open  -> close", &r->session},
        {"all in      close -> close", &r->close_to_close},
    };
    std::printf("        %-27s %10s %9s %8s %12s\n",
                "", "mean bps", "sd", "t", "total bps");
    double on_mean = 0.0, fb_mean = 0.0, hold_mean = 0.0;
    for (const Row& row : rows) {
        double m = 0.0, sd = 0.0, t = 0.0;
        mean_sd_t(*row.v, m, sd, t);
        double total = 0.0;
        for (const double x : *row.v) { total += x; }
        std::printf("        %-27s %+10.4f %9.2f %+8.2f %+12.1f\n",
                    row.name, m, sd, t, total);
        if (row.v == &r->overnight) { on_mean = m; }
        if (row.v == &r->first_bar) { fb_mean = m; }
        if (row.v == &r->close_to_close) { hold_mean = m; }
    }
    check(on_mean > 0.0 && hold_mean > 0.0,
          "both the overnight and the all-in means are positive");

    std::printf("\n        The first five minutes of the session, alone: "
                "%+.4f bps.\n", fb_mean - on_mean);
    std::printf("        That is the gap beginning to fill, and it is %.0f%% "
                "of the\n        overnight move. It is also exactly what you "
                "pay for being\n        unable to trade the pre-open auction."
                "\n",
                100.0 * (on_mean - fb_mean) / on_mean);
    check(fb_mean < on_mean,
          "the proxy boundary measures LESS than the true one, so the gap "
          "does start filling immediately");

    // -----------------------------------------------------------------------
    // 3. The strategy, at both exits.
    // -----------------------------------------------------------------------
    std::printf("\n[3] the strategy, and the cost it can bear\n");
    OvernightSpec at_open;
    OvernightSpec at_first;
    at_first.exit = Exit::FirstBar;
    const auto ro = run_overnight(bars, at_open);
    const auto rf = run_overnight(bars, at_first);
    check(ro.has_value() && rf.has_value(), "both exits produced results");
    if (!ro || !rf) { return 1; }

    std::printf("        %-22s %8s %8s %8s %14s %14s\n", "exit", "mean",
                "t", "hit", "breakeven RT", "vs buy-hold");
    const struct { const char* n; const OvernightResult* r; } both[] = {
        {"09:15 auction print", &*ro},
        {"09:20 first bar", &*rf},
    };
    for (const auto& e : both) {
        std::printf("        %-22s %+8.4f %+8.2f %8.3f %+14.4f %+14.4f\n",
                    e.n, e.r->mean_taken_bps, e.r->t_taken, e.r->hit_rate,
                    e.r->breakeven_rt_bps(),
                    e.r->breakeven_rt_vs_hold_bps());
    }
    check(ro->t_taken > 3.0,
          "the overnight mean at the auction print is strongly significant");
    check(rf->breakeven_rt_bps() < ro->breakeven_rt_bps(),
          "and exiting five minutes late lowers the cost it can bear");

    // The identity, on real data.
    std::printf("\n        identity check: breakeven vs hold should equal the "
                "session drag\n");
    std::printf("        breakeven vs hold %+.4f   -session %+.4f\n",
                ro->breakeven_rt_vs_hold_bps(), -ro->session_bps);
    check(std::fabs(ro->breakeven_rt_vs_hold_bps() + ro->session_bps) < 1e-9,
          "taking every night, the bar to beat holding IS the session drag");

    // -----------------------------------------------------------------------
    // 3b. THE SHAPE OF THE PAYOFF.
    //
    // A 67% hit rate and a positive mean can describe a strategy that grinds
    // out small wins or one that sells insurance, and the difference is the
    // skew. Here the MEDIAN night pays MORE than the mean night, which settles
    // it: most nights pay a little, a few take a lot back.
    // -----------------------------------------------------------------------
    std::printf("\n[3b] what the distribution of a night looks like\n");
    std::printf("        mean %+8.3f   median %+8.3f   sd %8.3f\n",
                ro->mean_taken_bps, ro->median_bps, ro->sd_taken_bps);
    std::printf("        p05  %+8.3f   p95    %+8.3f   worst %+8.1f\n",
                ro->p05_bps, ro->p95_bps, ro->worst_bps);
    std::printf("        a 5th-percentile night gives back %.1f average "
                "nights;\n        the worst one gives back %.0f.\n",
                ro->nights_per_bad_night(),
                ro->mean_taken_bps > 0.0
                    ? -ro->worst_bps / ro->mean_taken_bps : 0.0);
    check(ro->median_bps > ro->mean_taken_bps,
          "the median night pays MORE than the mean: the tail is on the "
          "downside");
    check(ro->p05_bps < 0.0 && ro->p95_bps > 0.0,
          "the distribution straddles zero at both tails, as prices do");
    std::printf("        THIS STRATEGY IS SHORT GAP RISK. It collects a\n"
                "        premium most nights for wearing the occasional\n"
                "        overnight shock, and its Sharpe will flatter it\n"
                "        right up until one arrives.\n");

    // -----------------------------------------------------------------------
    // 3c. IS IT ONE EPISODE, AND IS IT STILL HAPPENING?
    //
    // The two questions a full-sample mean cannot answer, and the two that
    // decide whether any of this is worth acting on.
    // -----------------------------------------------------------------------
    std::printf("\n[3c] by calendar year\n");
    const auto years = by_year(bars);
    check(years.has_value(), "the per-year breakdown was produced");
    if (years) {
        // THE LAST COLUMN IS THE ONE TO READ.
        //
        // It is the round trip that YEAR could have borne against buy-and-hold
        // -- and by the identity it is exactly minus that year's session drag.
        // The first version of this table printed a boolean "margin > 0"
        // instead, which said the same thing in all twelve rows: the session
        // was a drag in every year, so the boolean could not fail and was
        // therefore not a test. The number can.
        std::printf("        %6s %6s %11s %8s %11s %11s %13s\n", "year", "n",
                    "overnight", "t", "session", "close-close",
                    "RT it bears");
        std::size_t positive = 0, identity_ok = 0;
        double worst_margin = 1e300;
        const char* worst_year = "";
        for (const PeriodStat& ps : *years) {
            if (ps.overnight_bps > 0.0) { ++positive; }
            const double margin = ps.breakeven_vs_hold_bps();
            if (std::fabs(margin + ps.session_bps) < 1e-9) { ++identity_ok; }
            if (margin < worst_margin) {
                worst_margin = margin;
                worst_year = ps.label.c_str();
            }
            std::printf("        %6s %6zu %+11.3f %+8.2f %+11.3f %+11.3f "
                        "%+13.3f\n",
                        ps.label.c_str(), ps.n, ps.overnight_bps,
                        ps.overnight_t, ps.session_bps, ps.hold_bps, margin);
        }
        check(identity_ok == years->size(),
              "in every single year the bearable round trip equals the "
              "session drag, to 1e-9");
        std::printf("\n        Thinnest year: %s, which could bear a round "
                    "trip of only\n        %.3f bps. A NIFTY futures round "
                    "trip is dominated by\n        sell-side STT and is not "
                    "that small.\n", worst_year, worst_margin);
        std::printf("\n        %zu of %zu years had a positive overnight "
                    "mean.\n", positive, years->size());
        check(positive * 2 > years->size(),
              "the effect is present in a majority of years, not one episode");

        // THE DECAY QUESTION. Compare the last three years against the rest.
        double recent = 0.0, older = 0.0;
        std::size_t rn = 0, on_ = 0;
        for (std::size_t i = 0; i < years->size(); ++i) {
            const bool late = i + 3 >= years->size();
            const double w = static_cast<double>((*years)[i].n);
            if (late) { recent += (*years)[i].overnight_bps * w; rn += (*years)[i].n; }
            else      { older  += (*years)[i].overnight_bps * w; on_ += (*years)[i].n; }
        }
        if (rn > 0 && on_ > 0) {
            recent /= static_cast<double>(rn);
            older /= static_cast<double>(on_);
            std::printf("        first %zu years  %+8.3f bps/night\n"
                        "        last  3 years  %+8.3f bps/night\n",
                        years->size() - 3, older, recent);
            std::printf("        %s\n",
                        recent < older * 0.6
                            ? "THE EFFECT HAS DECAYED. The recent window is a\n"
                              "        different number from the full-sample "
                              "one, and it is\n        the recent one a "
                              "decision today would be made on."
                            : "The recent window is broadly in line with the "
                              "rest.");
        }
    }

    // -----------------------------------------------------------------------
    // 4. The sweep. No verdict, because no rate is hard-coded here.
    // -----------------------------------------------------------------------
    std::printf("\n[4] net per session against an all-in round trip\n");
    std::printf("        Bring the real rate from config/charges.toml. For "
                "NIFTY futures the\n        round trip is dominated by STT on "
                "the SELL side, which this\n        strategy pays every single "
                "morning.\n\n");
    std::printf("        %8s %12s %12s %12s %12s\n", "RT bps",
                "net (open)", "exc (open)", "net (09:20)", "exc (09:20)");
    bool crossed_open = false, crossed_first = false;
    for (const double rt : {0.0, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0}) {
        std::printf("        %8.1f %+12.4f %+12.4f %+12.4f %+12.4f\n",
                    rt, ro->net_at(rt), ro->excess_at(rt),
                    rf->net_at(rt), rf->excess_at(rt));
        if (ro->excess_at(rt) < 0.0) { crossed_open = true; }
        if (rf->excess_at(rt) < 0.0) { crossed_first = true; }
    }
    check(crossed_open && crossed_first,
          "both exits stop beating buy-and-hold somewhere inside the sweep");

    // -----------------------------------------------------------------------
    // 5. Does selecting nights raise the conditional mean?
    // -----------------------------------------------------------------------
    std::printf("\n[5] filtering on the preceding session -- does it raise "
                "the mean?\n");
    std::printf("        It cannot help by trading less: break-even against "
                "zero is the\n        conditional mean whatever the frequency."
                " It helps only if the\n        nights it keeps are better "
                "ones.\n\n");
    std::printf("        %14s %8s %8s %10s %14s %14s\n", "session below",
                "took", "share", "mean", "breakeven RT", "vs buy-hold");
    std::size_t rows_done = 0;
    double best_mean = ro->mean_taken_bps;
    const double kAll = std::numeric_limits<double>::infinity();
    for (const double th : {kAll, 100.0, 50.0, 0.0, -50.0, -100.0}) {
        OvernightSpec spec;
        spec.session_below_bps = th;
        const auto res = run_overnight(bars, spec);
        if (!res || res->taken == 0) { continue; }
        ++rows_done;
        char label[16];
        if (std::isinf(th)) {
            std::snprintf(label, sizeof label, "%s", "every night");
        } else {
            std::snprintf(label, sizeof label, "%+.1f", th);
        }
        std::printf("        %14s %8zu %7.1f%% %+10.4f %+14.4f %+14.4f\n",
                    label, res->taken,
                    100.0 * static_cast<double>(res->taken)
                        / static_cast<double>(res->sessions),
                    res->mean_taken_bps, res->breakeven_rt_bps(),
                    res->breakeven_rt_vs_hold_bps());
        best_mean = std::max(best_mean, res->mean_taken_bps);
    }
    check(rows_done >= 5, "the filter sweep produced results");
    std::printf("\n        best conditional mean in the sweep: %+.4f "
                "against %+.4f unfiltered.\n",
                best_mean, ro->mean_taken_bps);
    std::printf("        Six thresholds were tried, so a %.4f bps improvement "
                "is worth\n        roughly nothing without a walk-forward -- "
                "and this whole file is\n        in-sample over one eleven-year "
                "window.\n",
                best_mean - ro->mean_taken_bps);

    // -----------------------------------------------------------------------
    // 6. What would have to be true.
    // -----------------------------------------------------------------------
    std::printf("\n[6] what this result rests on\n");
    std::printf(
        "        1. FILLING AT THE 09:15 AUCTION PRINT. This is the whole\n"
        "           result. Against buy-and-hold -- the only comparison that\n"
        "           decides anything -- the round trip must come in under\n"
        "           %.2f bps if you hit the auction, and under %.2f bps if\n"
        "           you exit five minutes later instead. A NIFTY futures\n"
        "           round trip is dominated by sell-side STT and does not\n"
        "           fit under the second number.\n"
        "        2. Filling at the 15:30 close, which is a half-hour VWAP\n"
        "           construction rather than a price on the screen at 15:30.\n"
        "           NOT MODELLED HERE AT ALL, and it moves the same way.\n"
        "        3. Trading the index, which is not tradeable -- a future or\n"
        "           an ETF is, each with its own basis and roll.\n"
        "        4. In-sample, %zu sessions, one window, no walk-forward and\n"
        "           no purge.\n"
        "        5. And a stationary effect, which section 3c says it is not.\n",
        ro->breakeven_rt_vs_hold_bps(), rf->breakeven_rt_vs_hold_bps(),
        ro->sessions);
    std::printf(
        "\n        The gross edge is real and significant at t %+.2f, and it\n"
        "        is the first positive result in this project. It is also the\n"
        "        one most likely to be an artefact of a price nobody can\n"
        "        actually get: the difference between the two numbers above\n"
        "        is five minutes of the trading day.\n", ro->t_taken);

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
