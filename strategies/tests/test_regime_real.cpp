// strategies/tests/test_regime_real.cpp -- the regime detector on 36 years of
// real NIFTY, not on a synthetic series with a regime built into it.
//
// P6-03b.
//
// `test_regime.cpp` (P6-03) builds a series that HAS two regimes and checks the
// detector finds them. That is the right unit test and it settles nothing about
// whether the market has regimes a causal detector can see in time to trade.
// The Models page has said "daily NIFTY is sufficient; not yet fitted" since
// the card shipped.
//
// THE QUESTION A REGIME LABEL HAS TO ANSWER.
//
// Not "can I partition history into calm and stormy stretches" -- of course you
// can, and the partition will look compelling drawn on a chart with the answer
// already known. The question is whether a label available AT BAR t says
// anything about what happens AFTER bar t. A regime that describes the past
// beautifully and predicts nothing is a narrative, and sizing on it is sizing
// on a story.
//
// So this test conditions FORWARD returns on the label, with the forward window
// starting strictly after the bar the call was made on. Rule 7: no look-ahead,
// ever. The detector is causal by construction (push, then call) and the
// arithmetic here must not undo that.
//
// AND `Unknown` IS A RESULT, NOT A GAP.
//
// The detector publishes a label only when the statistic is `min_sigma` from
// the threshold AND has held for `min_bars`. On a real series most bars will
// fail one of those, and the fraction that do is the most useful number in the
// output: a detector that is confident every day is not measuring evidence.
// It is reported first, before any conditional return, because a conditional
// mean over 3% of the sample is a different claim from one over 60%.

#include <strategies/regime.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// Closes from the daily CSV, in paise. Its own reader, like every other
/// model test here: two independent paths to one series is what makes a
/// result a property of the data rather than of one parser.
std::vector<double> load_closes(const std::string& path) {
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);                       // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        double close = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 4 && !cell.empty()) { close = std::atof(cell.c_str()); }
            ++col;
        }
        if (close > 0.0) { out.push_back(close); }
    }
    return out;
}

/// Mean and standard error of a sample. Returned together because a mean
/// without its error is the number this repository keeps refusing to size on.
struct MeanSe {
    double mean = 0.0;
    double se = 0.0;
    std::size_t n = 0;
};

[[nodiscard]] MeanSe mean_se(const std::vector<double>& v) {
    MeanSe m;
    m.n = v.size();
    if (v.empty()) { return m; }
    double s = 0.0;
    for (const double x : v) { s += x; }
    m.mean = s / static_cast<double>(v.size());
    if (v.size() < 2) { return m; }
    double ss = 0.0;
    for (const double x : v) {
        const double d = x - m.mean;
        ss += d * d;
    }
    const double var = ss / static_cast<double>(v.size() - 1);
    m.se = std::sqrt(var / static_cast<double>(v.size()));
    return m;
}

/// Episode lengths: how many consecutive bars each spell of a label lasted.
struct Dwell {
    std::size_t episodes = 0;
    std::size_t median = 0;
    std::size_t longest = 0;
};

[[nodiscard]] Dwell dwell_of(std::vector<std::size_t> lengths) {
    Dwell d;
    d.episodes = lengths.size();
    if (lengths.empty()) { return d; }
    std::sort(lengths.begin(), lengths.end());
    d.median = lengths[lengths.size() / 2];
    d.longest = lengths.back();
    return d;
}

constexpr std::size_t kWindow = 256;   // bars of returns per Hurst estimate
constexpr std::size_t kFwd = 20;       // ~1 trading month ahead

} // namespace

int main() {
    using altair::Price;
    using altair::TrendDetector;
    using altair::TrendRegime;
    using altair::TrendThresholds;
    using altair::VolDetector;
    using altair::VolRegime;
    using altair::VolThresholds;

    std::printf("P6-03b regime detector on the real NIFTY daily series\n");

    const std::string path =
        std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d/all.csv";
    const auto closes = load_closes(path);
    if (closes.size() < 3000) {
        // dataset/ is gitignored and regenerable, so an absent series is a
        // loud skip rather than a failure -- but it is NOT silent, because
        // "no output" and "no regimes found" must not look the same.
        std::printf("  SKIP: %s has %zu closes; need 3000+\n",
                    path.c_str(), closes.size());
        return 0;
    }
    std::printf("  %zu daily closes\n\n", closes.size());

    // Forward return over the NEXT kFwd bars, in basis points. Index i holds
    // the return from close[i] to close[i + kFwd] -- strictly after the bar a
    // call is made on, which is what keeps rule 7.
    std::vector<double> fwd(closes.size(), 0.0);
    for (std::size_t i = 0; i + kFwd < closes.size(); ++i) {
        fwd[i] = 10000.0 * (closes[i + kFwd] / closes[i] - 1.0);
    }

    // ---- 1. HOW OFTEN DOES IT ACTUALLY DECIDE? ----------------------------
    //
    // Reported before any conditional mean, because a conditional mean over 3%
    // of the sample is a different claim from one over 60% and the reader has
    // to see the denominator first.
    TrendThresholds tt;
    tt.centre = 0.5;
    tt.min_sigma = 2.0;        // 2 sigma to claim a side
    tt.exit_sigma = 1.0;       // 1 sigma to keep it -- the hysteresis
    tt.min_bars = 5;

    TrendDetector<kWindow> trend;
    std::vector<TrendRegime> label(closes.size(), TrendRegime::Unknown);
    std::size_t decided = 0, refused = 0;
    std::size_t trend_bars = 0, revert_bars = 0, walk_bars = 0;

    for (std::size_t i = 0; i < closes.size(); ++i) {
        trend.push(Price{static_cast<std::int64_t>(closes[i])});
        const auto c = trend.call(tt);
        if (!c) { ++refused; continue; }
        label[i] = c->label;
        if (c->decided()) { ++decided; }
        switch (c->label) {
        case TrendRegime::Trending:      ++trend_bars;  break;
        case TrendRegime::MeanReverting: ++revert_bars; break;
        case TrendRegime::RandomWalk:    ++walk_bars;   break;
        case TrendRegime::Unknown:                      break;
        }
    }

    const double decided_pct =
        100.0 * static_cast<double>(decided) / static_cast<double>(closes.size());
    std::printf("  TREND, Hurst over a %zu-bar window, %.1f sigma to enter /"
                " %.1f to hold, %zu-bar dwell\n",
                kWindow, tt.min_sigma, tt.exit_sigma, tt.min_bars);
    std::printf("    decided on %zu of %zu bars (%.1f%%);"
                " %zu calls refused for want of data\n",
                decided, closes.size(), decided_pct, refused);

    check(decided < closes.size(),
          "the detector does NOT publish a label every bar -- one that did "
          "would be reporting a threshold crossing, not evidence");
    check(walk_bars > trend_bars + revert_bars,
          "and the label it publishes most is RANDOM WALK, which regime.hpp "
          "calls the honest answer -- a detector that rarely says it is "
          "describing its own noise");

    // ---- 2. DWELL: IS AN EPISODE LONG ENOUGH TO TRADE? --------------------
    //
    // A regime that flips every three bars cannot be traded through a 5.5 bps
    // round trip whatever it predicts, so this is a cost question before it is
    // a statistics question.
    std::vector<std::size_t> spells;
    TrendRegime run_label = TrendRegime::Unknown;
    std::size_t run_len = 0;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (label[i] == run_label) {
            ++run_len;
        } else {
            if (run_label != TrendRegime::Unknown && run_len > 0) {
                spells.push_back(run_len);
            }
            run_label = label[i];
            run_len = 1;
        }
    }
    if (run_label != TrendRegime::Unknown && run_len > 0) {
        spells.push_back(run_len);
    }
    const Dwell d = dwell_of(spells);
    std::printf("    %zu labelled episodes, median %zu bars, longest %zu\n\n",
                d.episodes, d.median, d.longest);

    // ---- 3. THE ONLY QUESTION THAT MATTERS --------------------------------
    //
    // Does a label available at bar t say anything about bars t+1 .. t+kFwd?
    // EVERY ENUMERATOR EXPLICITLY, and RandomWalk gets its own row.
    //
    // The first draft wrote `default:` here and folded RandomWalk in with
    // Unknown. That contradicted the line three inches above it -- 8,216
    // "decided" against 8,147 "Unknown" -- and it buried the largest number in
    // the output. regime.hpp says why in its own words: "A RANDOM WALK IS A
    // FINDING, not a failure to classify... a detector that never returns it
    // is describing its own noise." A `default:` in a switch over a regime
    // label is how that finding gets thrown away.
    std::vector<double> f_trend, f_mean, f_walk, f_unknown;
    for (std::size_t i = 0; i + kFwd < closes.size(); ++i) {
        switch (label[i]) {
        case TrendRegime::Trending:      f_trend.push_back(fwd[i]);   break;
        case TrendRegime::MeanReverting: f_mean.push_back(fwd[i]);    break;
        case TrendRegime::RandomWalk:    f_walk.push_back(fwd[i]);    break;
        case TrendRegime::Unknown:       f_unknown.push_back(fwd[i]); break;
        }
    }
    const MeanSe mt = mean_se(f_trend);
    const MeanSe mm = mean_se(f_mean);
    const MeanSe mw = mean_se(f_walk);
    const MeanSe mu = mean_se(f_unknown);

    std::printf("  FORWARD %zu-BAR RETURN, conditioned on the label AT bar t\n",
                kFwd);
    std::printf("    %-16s %8s %12s %12s\n", "label", "n", "mean bps", "se");
    std::printf("    %-16s %8zu %12.2f %12.2f\n", "Trending",
                mt.n, mt.mean, mt.se);
    std::printf("    %-16s %8zu %12.2f %12.2f\n", "MeanReverting",
                mm.n, mm.mean, mm.se);
    std::printf("    %-16s %8zu %12.2f %12.2f   <-- the honest majority\n",
                "RandomWalk", mw.n, mw.mean, mw.se);
    std::printf("    %-16s %8zu %12.2f %12.2f\n", "Unknown (no call)",
                mu.n, mu.mean, mu.se);

    // The separation between the two decided labels, in standard errors of
    // the DIFFERENCE. Sizing on the point estimate is what ROADMAP section 3
    // forbids; this is the lower-confidence-bound question in its simplest
    // form.
    double sep = 0.0;
    if (mt.n > 1 && mm.n > 1) {
        const double se_diff = std::sqrt(mt.se * mt.se + mm.se * mm.se);
        if (se_diff > 0.0) { sep = (mt.mean - mm.mean) / se_diff; }
        std::printf("\n    Trending minus MeanReverting: %.2f bps,"
                    " %.2f sigma\n", mt.mean - mm.mean, sep);
    }

    check(mt.n > 100 && mm.n > 100,
          "both decided labels occur often enough for the comparison to mean "
          "something");

    // ---- 4. WHAT THE HYSTERESIS BUYS --------------------------------------
    //
    // exit_sigma is the only parameter here that exists purely to reduce
    // churn, so its effect is measured rather than assumed. Same series, same
    // entry threshold, hysteresis removed.
    TrendThresholds flat = tt;
    flat.exit_sigma = tt.min_sigma;     // leave as readily as you enter
    TrendDetector<kWindow> trend2;
    std::size_t flips_hyst = 0, flips_flat = 0;
    std::size_t sided_hyst = 0, sided_flat = 0;   // Trending or MeanReverting
    TrendRegime prev2 = TrendRegime::Unknown;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        trend2.push(Price{static_cast<std::int64_t>(closes[i])});
        const auto c = trend2.call(flat);
        const TrendRegime l = c ? c->label : TrendRegime::Unknown;
        if (l != prev2) { ++flips_flat; }
        if (l == TrendRegime::Trending || l == TrendRegime::MeanReverting) {
            ++sided_flat;
        }
        prev2 = l;
    }
    TrendRegime prev1 = TrendRegime::Unknown;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (label[i] != prev1) { ++flips_hyst; }
        if (label[i] == TrendRegime::Trending
            || label[i] == TrendRegime::MeanReverting) {
            ++sided_hyst;
        }
        prev1 = label[i];
    }

    // COUNTING FLIPS ALONE IS THE WRONG MEASURE, and the first draft asserted
    // on it and failed: 193 changes with the hysteresis against 139 without.
    // That looks like hysteresis making things worse, and it is not.
    //
    // `exit_sigma` interacts with `min_bars`. A readier exit throws the
    // statistic out of a side before the 5-bar dwell is satisfied, so the
    // label is never PUBLISHED at all -- the detector sits in RandomWalk and
    // changes state less often while saying less. Fewer changes bought by
    // refusing to speak is not less churn, it is less information.
    //
    // So the metric is CHANGES PER SIDED BAR: how much churn you pay for each
    // bar on which the detector actually took a side. That is the quantity a
    // strategy gated on this label would feel, because every change is a
    // position change and every sided bar is the exposure it bought.
    const double churn_hyst =
        sided_hyst > 0 ? static_cast<double>(flips_hyst)
                             / static_cast<double>(sided_hyst) : 0.0;
    const double churn_flat =
        sided_flat > 0 ? static_cast<double>(flips_flat)
                             / static_cast<double>(sided_flat) : 0.0;
    std::printf("\n  HYSTERESIS\n");
    std::printf("    exit at %.1f sigma: %zu changes over %zu sided bars"
                " -> %.4f changes/sided bar\n",
                tt.exit_sigma, flips_hyst, sided_hyst, churn_hyst);
    std::printf("    exit at %.1f sigma: %zu changes over %zu sided bars"
                " -> %.4f changes/sided bar\n",
                flat.exit_sigma, flips_flat, sided_flat, churn_flat);
    check(churn_hyst <= churn_flat,
          "asymmetric exit costs no more churn PER SIDED BAR -- the raw flip "
          "count is not the measure, because a readier exit also stops the "
          "detector publishing at all");

    // ---- 5. VOLATILITY REGIME ---------------------------------------------
    //
    // Thresholds in paise of daily standard deviation. NIFTY spans 250 to
    // 26,000 over this series, so a fixed paise threshold is meaningless
    // across it -- the regime is therefore read on the RETURN series scale by
    // setting thresholds from the sample itself, and the fact that they cannot
    // be constants is the finding.
    VolDetector<60> vol;
    std::vector<double> sds;
    VolThresholds probe;
    probe.low = 0.0; probe.high = 1e18; probe.min_sigma = 0.5; probe.min_bars = 1;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        vol.push(Price{static_cast<std::int64_t>(closes[i])});
        const auto c = vol.call(probe);
        if (c) { sds.push_back(c->statistic); }
    }
    if (!sds.empty()) {
        std::vector<double> s = sds;
        std::sort(s.begin(), s.end());
        const double q25 = s[s.size() / 4];
        const double q75 = s[3 * s.size() / 4];
        std::printf("\n  VOL, 60-bar realised sd in paise: q25 %.0f,"
                    " median %.0f, q75 %.0f, max %.0f\n",
                    q25, s[s.size() / 2], q75, s.back());
        std::printf("    the q75/q25 ratio is %.1fx across the series --"
                    " a FIXED paise threshold\n"
                    "    cannot separate 1990 from 2026, so vol thresholds"
                    " have to be quantiles\n"
                    "    of a trailing window, not constants in a config"
                    " file\n", q75 / (q25 > 0.0 ? q25 : 1.0));
        check(q75 > q25,
              "the realised-vol distribution is wide enough that a regime "
              "split is a real question rather than a rounding artefact");
    }

    // ---- VERDICT ----------------------------------------------------------
    std::printf("\n  VERDICT\n");
    std::printf("    The detector is causal, it refuses to label %.0f%% of"
                " bars, and its\n"
                "    episodes have a median length of %zu bars.\n",
                100.0 - decided_pct, d.median);
    if (std::fabs(sep) < 2.0) {
        std::printf("\n    THE LABEL DOES NOT SEPARATE FORWARD RETURNS."
                    " Trending minus\n"
                    "    MeanReverting is %.2f bps at %.2f sigma over %zu"
                    " bars ahead. This\n"
                    "    is a DESCRIPTION of the past, and sizing on it would"
                    " be sizing on a\n"
                    "    story. It may still be useful as a GATE -- refusing"
                    " to trade a\n"
                    "    strategy outside the regime it was fitted in -- which"
                    " is a different\n"
                    "    claim, and one this test does not make.\n",
                    mt.mean - mm.mean, sep, kFwd);
    } else {
        std::printf("\n    The label separates forward returns at %.2f sigma."
                    " Before this is\n"
                    "    sized, it must clear cost (rule 5) and survive"
                    " walk-forward: a single\n"
                    "    in-sample split over 36 years is not evidence of a"
                    " tradeable edge.\n", sep);
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
