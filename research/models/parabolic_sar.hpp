// research/models/parabolic_sar.hpp -- Wilder's Parabolic SAR ("stop and reverse").
//
// J. Welles Wilder, New Concepts in Technical Trading Systems (1978). Added
// for A. Fauza & E. Noviaty, "Analysis of Accuracy Level of Moving Average,
// Parabolic SAR and Convolutional Neural Network Indicators on Buy and Sell
// Decisions" (Jurnal Ekonomi 11(3), December 2022), which compares the three
// on one Indonesian bank stock for 2021.
//
// THE RULE. In an uptrend the stop sits below price and rises each bar:
//     SAR' = SAR + AF (EP - SAR)
// where EP is the highest high of the trend and AF the acceleration factor,
// starting at `af_start`, raised by `af_step` at every new EP, capped at
// `af_max` (Wilder: 0.02, 0.02, 0.20). The stop may not rise above either of
// the two previous lows. When a low breaks it, the trend reverses: the new
// stop is the old EP, EP becomes that bar's low, AF resets. Mirror image in a
// downtrend.
//
// sar[i] is the stop in force DURING bar i, computed from bars before it;
// trend[i] is the side the rule holds after bar i (a reversal happens on the
// bar whose range crosses its stop). Acting on trend[i] from bar i+1 uses
// nothing from the future.

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace altair {

struct PsarConfig {
    double af_start = 0.02;
    double af_step = 0.02;
    double af_max = 0.20;
};

struct PsarSeries {
    std::vector<double> sar;    ///< the stop in force during each bar
    std::vector<int> trend;     ///< +1 long, -1 short, after each bar
};

[[nodiscard]] inline PsarSeries
parabolic_sar(std::span<const double> high, std::span<const double> low, PsarConfig cfg = {}) {
    PsarSeries out;
    const std::size_t n = std::min(high.size(), low.size());
    if (n < 2) { return out; }
    out.sar.assign(n, 0.0);
    out.trend.assign(n, 0);
    // Start in the direction of the first move.
    int up = high[1] + low[1] >= high[0] + low[0] ? 1 : -1;
    double ep = up > 0 ? std::max(high[0], high[1]) : std::min(low[0], low[1]);
    double sar = up > 0 ? std::min(low[0], low[1]) : std::max(high[0], high[1]);
    double af = cfg.af_start;
    bool fresh = false;   // a reversal just set the stop to the old EP
    out.sar[0] = out.sar[1] = sar;
    out.trend[0] = out.trend[1] = up;
    for (std::size_t i = 2; i < n; ++i) {
        // The stop in force during bar i, from bars before it.
        double stop = fresh ? sar : sar + af * (ep - sar);
        stop = up > 0 ? std::min({stop, low[i - 1], low[i - 2]})
                      : std::max({stop, high[i - 1], high[i - 2]});
        out.sar[i] = stop;
        fresh = false;
        if (up > 0 && low[i] < stop) {          // stopped out: reverse to short
            up = -1;
            sar = ep;
            ep = low[i];
            af = cfg.af_start;
            fresh = true;
        } else if (up < 0 && high[i] > stop) {  // stopped out: reverse to long
            up = 1;
            sar = ep;
            ep = high[i];
            af = cfg.af_start;
            fresh = true;
        } else {
            sar = stop;
            if (up > 0 && high[i] > ep) { ep = high[i]; af = std::min(cfg.af_max, af + cfg.af_step); }
            if (up < 0 && low[i] < ep)  { ep = low[i];  af = std::min(cfg.af_max, af + cfg.af_step); }
        }
        out.trend[i] = up;
    }
    return out;
}

} // namespace altair
