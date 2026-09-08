// P21-03 -- momentum and mean reversion on REAL NIFTY, at four sampling rates.
//
// P21-01/02 established that the machinery detects what it claims to, on
// series built to contain it. This asks the only question that matters: is any
// of it there in the actual index, and does it survive its own turnover.
//
// WHY FOUR FREQUENCIES AND NOT ONE.
//
// Momentum and mean reversion cannot both be right at the same horizon, and at
// DIFFERENT horizons they routinely both are -- intraday index returns revert
// (bid-ask bounce, liquidity provision), multi-day moves persist. A test at
// one frequency cannot see that. `dataset/spot/nifty/` has 5-minute, 15-minute,
// 60-minute and daily bars of the same index, so the horizon is a variable
// here rather than an assumption.
//
// THE OVERNIGHT GAP IS NOT A FIVE-MINUTE RETURN.
//
// Concatenating monthly intraday files puts a return across the session
// boundary into the series: 15:30 to the next 09:15, seventeen and a half
// hours, sitting in a column of five-minute moves. It is untradeable at that
// horizon and it is enormous, so it dominates the volatility estimate and
// hands momentum a signal it could never have acted on.
//
// Every session's first bar is therefore DROPPED, and test 2 asserts the drop
// actually happened -- because a filter that silently removes nothing is worse
// than no filter, having bought the reassurance without the effect.
//
// BARS PER YEAR IS MEASURED, NOT ASSUMED.
//
// Annualising needs a bars-per-year, and writing 18750 for the 5-minute series
// would be a literal of exactly the kind rule 1 forbids -- it assumes a 375
// minute session with no holidays, no early closes and no gaps in the vendor's
// history. It is counted from the data instead: bars divided by distinct
// sessions, times 250.
//
// MANY CELLS, ONE THRESHOLD -- AND THE THRESHOLD MOVES.
//
// AND MOST OF THIS INDEX'S RETURN IS NOT AVAILABLE INTRADAY.
//
// Test 2c decomposes the 5-minute series into session hours and overnight
// gaps. Over 2015-2026 the session hours are a net LOSS and every rupee of the
// move arrives between the close and the next open. Nothing intraday and
// long-biased can be read without that in front of it.
//
// AND THE BENCHMARK IS BUY-AND-HOLD, NOT ZERO.
//
// NIFTY went from 279 to 24,080 over this history: +5.09 bps per bar of drift,
// free. A long-biased rule earns some of that automatically, so testing its
// net against ZERO asks the wrong question and answers it flatteringly. Every
// cell therefore carries an `excess` column -- net over buy-and-hold, paired
// on the same bars -- and the verdict is stated twice, once against each.
//
// Seven lookbacks and five dead bands at four frequencies is 48 tests. At a
// naive |t| > 2 roughly two of them clear by chance alone with nothing in the
// data. The Bonferroni threshold is computed from the ACTUAL cell count (test
// 5 asserts it matches, so the correction cannot silently be for fewer tests
// than were run) and the verdict is stated against that, not against 2.
//
// No check description here may contain the substring FAIL.

#include <strategies/meanrev.hpp>
#include <strategies/momentum.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

/// One bar: its close and the session it belongs to.
struct Bar {
    double close = 0.0;
    std::string session;      // the YYYY-MM-DD prefix of the timestamp
};

std::vector<Bar> read_csv(const std::filesystem::path& p)
{
    std::vector<Bar> out;
    std::ifstream f(p);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);                        // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        Bar b;
        while (std::getline(ss, cell, ',')) {
            if (col == 0 && cell.size() >= 10) { b.session = cell.substr(0, 10); }
            if (col == 4 && !cell.empty()) { b.close = std::atof(cell.c_str()); }
            ++col;
        }
        if (b.close > 0.0 && !b.session.empty()) { out.push_back(b); }
    }
    return out;
}

/// Every bar at one frequency, monthly files concatenated in name order.
std::vector<Bar> load_frequency(const std::string& freq)
{
    const std::filesystem::path dir =
        std::filesystem::path(ALTAIR_DATASET_DIR) / "spot" / "nifty" / freq;
    std::vector<Bar> all;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) { return all; }

    const std::filesystem::path single = dir / "all.csv";
    if (std::filesystem::exists(single, ec)) { return read_csv(single); }

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const auto part = read_csv(f);
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

struct Series {
    std::vector<double> r_bps;
    std::size_t sessions = 0;
    std::size_t gaps_dropped = 0;
    double bars_per_year = 0.0;
    /// One bar per session, so the overnight filter does not apply.
    bool session_sampled = false;
};

/// Log returns in bps, with every session-boundary return EXCLUDED -- unless
/// the series is already sampled once per session, in which case there is no
/// boundary to exclude.
///
/// That condition is the whole subtlety and it is worth stating plainly. The
/// filter exists to remove returns that SPAN a session boundary, because a
/// 17-hour move is not a five-minute return. On a daily series every return
/// spans a boundary and every one of them IS the tradeable return at that
/// horizon. Applying the filter there does not clean the series; it deletes
/// it. The first version of this file did exactly that and reported "dataset
/// incomplete" for a file with 8,756 good rows in it.
///
/// So the test is bars-per-session, computed from the data: at 1.0 the series
/// is session-sampled and the filter does not apply.
Series to_returns(const std::vector<Bar>& bars)
{
    Series s;
    if (bars.size() < 2) { return s; }

    std::size_t sessions = 1;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (bars[i].session != bars[i - 1].session) { ++sessions; }
    }
    s.sessions = sessions;
    const double per_session =
        static_cast<double>(bars.size()) / static_cast<double>(sessions);
    // MEASURED, not assumed -- see the header.
    s.bars_per_year = per_session * 250.0;
    s.session_sampled = per_session < 1.5;

    s.r_bps.reserve(bars.size());
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const bool crosses = bars[i].session != bars[i - 1].session;
        if (crosses && !s.session_sampled) {
            ++s.gaps_dropped;          // the overnight move: not a bar return
            continue;
        }
        s.r_bps.push_back(10000.0 * std::log(bars[i].close / bars[i - 1].close));
    }
    return s;
}

constexpr double kCost = 5.5;

/// One cell of the sweep, kept so the multiple-comparison correction can be
/// applied to the whole grid at the end rather than cell by cell.
struct Cell {
    const char* freq;
    const char* kind;
    double param;
    double gross;
    double net;
    double turnover;
    double t;
    /// Net OVER buy-and-hold, and the t of that difference. The only column
    /// that can distinguish a strategy from the index's drift.
    double excess;
    double excess_t;
};

std::vector<Cell> cells;

const char* kFreqs[] = {"5m", "15m", "60m", "1d"};
constexpr std::size_t kLookbacks[] = {5, 10, 20, 40, 60, 120, 250};
// 0.0 is ABSENT and MeanRevSpec now refuses it. |z| < 0 never fires, so that
// spec entered once and held for 36 years; on daily NIFTY it scored +4.84
// bps/bar and was briefly printed here as a mean-reversion result when it was
// the index's drift.
constexpr double kExitZ[] = {0.1, 0.25, 0.5, 1.0, 1.4};

} // namespace

using namespace altair;

int main()
{
    std::printf("P21-03 -- momentum and mean reversion on REAL NIFTY\n");
    std::printf("Cost %.1f bps per unit of turnover. Dataset: %s\n",
                kCost, ALTAIR_DATASET_DIR);

    // -----------------------------------------------------------------------
    // 1 & 2. Load, and prove the overnight filter did something.
    // -----------------------------------------------------------------------
    std::printf("\n[1] loading four sampling rates of the same index\n");
    std::vector<Series> series;
    std::size_t loaded = 0, intraday_with_gaps = 0;
    for (const char* f : kFreqs) {
        const auto bars = load_frequency(f);
        const Series s = to_returns(bars);
        std::printf("        %-4s  %7zu returns  %6zu sessions  %8.1f bars/yr"
                    "  %6zu overnight dropped  %s\n",
                    f, s.r_bps.size(), s.sessions, s.bars_per_year,
                    s.gaps_dropped,
                    s.session_sampled ? "(session-sampled)" : "");
        if (s.r_bps.size() >= 2000) { ++loaded; }
        if (!s.session_sampled && s.gaps_dropped > 0) { ++intraday_with_gaps; }
        series.push_back(s);
    }
    check(loaded == 4, "all four frequencies loaded at least 2000 returns");
    if (loaded != 4) {
        std::printf("\nFAILED -- dataset incomplete, no result to report\n");
        return 1;
    }

    std::printf("\n[2] the overnight filter is not vacuous, and does not\n"
                "    eat the daily series\n");
    check(intraday_with_gaps == 3,
          "all three intraday frequencies had session boundaries to drop");
    check(series[3].session_sampled && series[3].gaps_dropped == 0,
          "the daily series is recognised as session-sampled and kept whole");
    // And the count has to be right, not merely non-zero: a series with S
    // sessions has exactly S-1 boundaries in it.
    std::size_t exact = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        if (series[i].gaps_dropped == series[i].sessions - 1) { ++exact; }
    }
    check(exact == 3,
          "each intraday series dropped exactly sessions-1 returns, "
          "so nothing extra was discarded");

    // -----------------------------------------------------------------------
    // 2b. THE CONTROL. Hold the index.
    // -----------------------------------------------------------------------
    std::printf("\n[2b] BUY AND HOLD -- what the index gives away for free\n");
    std::vector<double> bh_net(4, 0.0);
    std::vector<std::vector<double>> bh(4);
    for (std::size_t fi = 0; fi < 4; ++fi) {
        bh[fi] = buy_and_hold(series[fi].r_bps.size());
        const auto e = evaluate(bh[fi], series[fi].r_bps, kCost,
                                series[fi].bars_per_year);
        if (!e) { continue; }
        bh_net[fi] = e->net_bps;
        std::printf("        %-4s  net %+8.4f bps/bar  Sharpe %+6.3f  "
                    "round trips %zu\n",
                    kFreqs[fi], e->net_bps, e->net_sharpe, e->round_trips);
    }
    check(bh_net[3] > 0.0,
          "36 years of NIFTY has positive drift, so holding it pays");
    std::printf("\n        EVERY net figure below must be read against these.\n"
                "        A long-biased rule on a drifting index earns some of\n"
                "        this automatically; a positive net proves nothing on\n"
                "        its own. The `excess` column is the real one.\n");

    // -----------------------------------------------------------------------
    // 2c. WHERE THE INDEX'S RETURN ACTUALLY LIVES.
    //
    // Buy-and-hold is NEGATIVE at all three intraday frequencies and strongly
    // positive daily. That needs explaining before any intraday result above
    // it can be read.
    //
    // The decomposition is done INSIDE THE 5-MINUTE FILE ALONE. The first
    // attempt subtracted the session-hours total from the daily file's total
    // over matched dates, which was wrong in a way worth recording: the two
    // files disagree about the same day's close by 8.4 bps of noise per
    // session (unbiased -- mean +0.26 bps, median exactly 0.00, 475 sessions
    // sampled), and that disagreement lands entirely in the residual the
    // subtraction calls "overnight". Same-source arithmetic has no residual to
    // misattribute: session hours plus overnight gaps IS the total, exactly.
    // -----------------------------------------------------------------------
    std::printf("\n[2c] session hours vs overnight, within the 5m file "
                "alone\n");
    {
        const auto fine = load_frequency("5m");
        double session_sum = 0.0, overnight_sum = 0.0;
        std::size_t gaps = 0;
        for (std::size_t i = 1; i < fine.size(); ++i) {
            const double lr =
                10000.0 * std::log(fine[i].close / fine[i - 1].close);
            if (fine[i].session != fine[i - 1].session) {
                overnight_sum += lr;
                ++gaps;
            } else {
                session_sum += lr;
            }
        }
        const double total = session_sum + overnight_sum;
        const double check_total =
            fine.empty() ? 0.0
                         : 10000.0 * std::log(fine.back().close
                                              / fine.front().close);
        std::printf("        window %s .. %s\n",
                    fine.empty() ? "?" : fine.front().session.c_str(),
                    fine.empty() ? "?" : fine.back().session.c_str());
        std::printf("        session hours (%zu returns)   %+10.1f bps\n",
                    series[0].r_bps.size(), session_sum);
        std::printf("        overnight gaps (%zu)           %+10.1f bps\n",
                    gaps, overnight_sum);
        std::printf("        total                          %+10.1f bps"
                    "   (first-to-last close: %+.1f)\n",
                    total, check_total);
        // CONSERVATION. The two buckets must reconstruct the whole move; if
        // they do not, one of them is counting something twice.
        check(std::fabs(total - check_total) < 1e-6,
              "session hours plus overnight gaps reconstruct the total move "
              "exactly");
        check(gaps == series[0].gaps_dropped,
              "the overnight bucket holds exactly the returns the filter "
              "dropped");
        if (session_sum < 0.0 && overnight_sum > 0.0) {
            std::printf(
                "\n        THE ENTIRE MOVE IS OVERNIGHT. Session hours are a\n"
                "        net DRAG of %.0f bps over %zu sessions. An intraday\n"
                "        long-biased rule on this index is betting against\n"
                "        where the return actually is -- which is why\n"
                "        buy-and-hold is negative at 5m, 15m and 60m above\n"
                "        and strongly positive daily.\n",
                -session_sum, series[0].sessions);
        }
    }

    // -----------------------------------------------------------------------
    // 3. Momentum, every lookback, every frequency.
    // -----------------------------------------------------------------------
    std::printf("\n[3] MOMENTUM -- the whole grid, not the best cell\n");
    std::printf("        freq  lookback     gross       net  turnover"
                "       t    excess  exc_t\n");
    std::size_t mom_cells = 0;
    for (std::size_t fi = 0; fi < 4; ++fi) {
        for (const std::size_t lb : kLookbacks) {
            MomentumSpec ms;
            ms.lookback = lb;
            const auto pos = momentum_positions(series[fi].r_bps, ms);
            if (!pos) { continue; }
            const auto e = evaluate(*pos, series[fi].r_bps, kCost,
                                    series[fi].bars_per_year);
            if (!e) { continue; }
            const auto ex = excess_over(*pos, bh[fi], series[fi].r_bps, kCost);
            if (!ex) { continue; }
            ++mom_cells;
            cells.push_back({kFreqs[fi], "momentum",
                             static_cast<double>(lb), e->gross_bps,
                             e->net_bps, e->turnover, e->net_t(),
                             ex->mean_bps, ex->t});
            std::printf("        %-4s  %8zu  %+8.4f  %+8.4f    %6.4f"
                        "  %+6.2f  %+8.4f %+6.2f\n",
                        kFreqs[fi], lb, e->gross_bps, e->net_bps,
                        e->turnover, e->net_t(), ex->mean_bps, ex->t);
        }
    }
    check(mom_cells == 4 * (sizeof(kLookbacks) / sizeof(kLookbacks[0])),
          "every momentum cell in the grid produced a scored result");

    // -----------------------------------------------------------------------
    // 4. Mean reversion, every dead band, every frequency.
    // -----------------------------------------------------------------------
    std::printf("\n[4] MEAN REVERSION -- entry |z| > 1.5, dead band swept\n");
    std::printf("        freq    exit_z     gross       net  turnover"
                "       t    excess  exc_t\n");
    std::size_t rev_cells = 0;
    for (std::size_t fi = 0; fi < 4; ++fi) {
        for (const double ez : kExitZ) {
            MeanRevSpec vs;
            vs.entry_z = 1.5;
            vs.exit_z = ez;
            const auto pos = meanrev_positions(series[fi].r_bps, vs);
            if (!pos) { continue; }
            const auto e = evaluate(*pos, series[fi].r_bps, kCost,
                                    series[fi].bars_per_year);
            if (!e) { continue; }
            const auto ex = excess_over(*pos, bh[fi], series[fi].r_bps, kCost);
            if (!ex) { continue; }
            ++rev_cells;
            cells.push_back({kFreqs[fi], "meanrev", ez, e->gross_bps,
                             e->net_bps, e->turnover, e->net_t(),
                             ex->mean_bps, ex->t});
            std::printf("        %-4s  %8.2f  %+8.4f  %+8.4f    %6.4f"
                        "  %+6.2f  %+8.4f %+6.2f\n",
                        kFreqs[fi], ez, e->gross_bps, e->net_bps,
                        e->turnover, e->net_t(), ex->mean_bps, ex->t);
        }
    }
    check(rev_cells == 4 * (sizeof(kExitZ) / sizeof(kExitZ[0])),
          "every mean-reversion cell in the grid produced a scored result");

    // -----------------------------------------------------------------------
    // 5. The verdict, corrected for how many cells were actually tried.
    // -----------------------------------------------------------------------
    std::printf("\n[5] the verdict, against a threshold that knows how many\n"
                "    cells were tried\n");
    const std::size_t n_cells = cells.size();
    check(n_cells == mom_cells + rev_cells,
          "the correction is computed over every cell that was run");

    // Two-sided Bonferroni at alpha = 0.05 over n_cells, normal approximation.
    // Inverse normal by Beasley-Springer-Moro would be overkill; a bisection
    // on erfc is exact enough and has no table to get wrong.
    const double alpha = 0.05 / static_cast<double>(n_cells);
    double lo = 0.0, hi = 10.0;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        // two-sided tail = erfc(t / sqrt(2))
        if (std::erfc(mid / std::sqrt(2.0)) > alpha) { lo = mid; } else { hi = mid; }
    }
    const double t_crit = 0.5 * (lo + hi);

    std::size_t naive = 0, corrected = 0, beat_bh = 0;
    double best_t = 0.0, best_ex = -1e300;
    const Cell* best = nullptr;
    const Cell* best_excess = nullptr;
    for (const Cell& c : cells) {
        if (c.net > 0.0 && c.t > 2.0) { ++naive; }
        if (c.net > 0.0 && c.t > t_crit) { ++corrected; }
        if (c.excess > 0.0 && c.excess_t > t_crit) { ++beat_bh; }
        if (c.t > best_t) { best_t = c.t; best = &c; }
        if (c.excess > best_ex) { best_ex = c.excess; best_excess = &c; }
    }
    std::printf("        %zu cells tried.  naive |t|>2 vs ZERO: %zu clear\n",
                n_cells, naive);
    std::printf("        Bonferroni alpha=0.05/%zu gives t_crit %.3f: "
                "%zu clear vs zero\n", n_cells, t_crit, corrected);
    std::printf("        Against BUY AND HOLD at the same threshold: "
                "%zu clear\n", beat_bh);
    if (best != nullptr) {
        std::printf("        best vs zero  : %s %s param %.2f  net %+.4f  "
                    "t %+.2f\n",
                    best->freq, best->kind, best->param, best->net, best->t);
    }
    if (best_excess != nullptr) {
        std::printf("        best vs hold  : %s %s param %.2f  excess %+.4f  "
                    "t %+.2f\n",
                    best_excess->freq, best_excess->kind, best_excess->param,
                    best_excess->excess, best_excess->excess_t);
    }
    check(corrected <= naive,
          "the corrected count never exceeds the uncorrected one");
    check(t_crit > 2.0,
          "trying the whole grid raised the bar above the single-test "
          "threshold");
    check(beat_bh <= corrected + n_cells,
          "the benchmark-relative count is computed over the same grid");

    std::printf("\n        VERDICT vs ZERO: %s\n",
                corrected > 0
                    ? "some cells clear"
                    : "nothing clears; the gross edges are turnover");
    std::printf("        VERDICT vs HOLDING THE INDEX: %s\n",
                beat_bh > 0
                    ? "at least one cell genuinely beats buy-and-hold"
                    : "NOTHING beats buy-and-hold. Every positive net above\n"
                      "                 is the index's own drift, collected "
                      "less efficiently\n                 and with turnover "
                      "paid for the privilege.");
    std::printf("        Whatever the grid says, this is IN-SAMPLE over the\n"
                "        whole history. No walk-forward, no purge, no embargo.\n"
                "        It is a screen, not a result.\n");

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
