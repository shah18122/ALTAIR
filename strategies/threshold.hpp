// strategies/threshold.hpp -- the owner's threshold strategies, as written in
// threshold_strategy/. Daily bars in, trades out. PAPER ONLY: this header
// detects; nothing here can place an order.
//
// 1. PREV-2-DAY BREAKOUT on BANKNIFTY ("2-Day High / Low -- BNF",
//    threshold_strategy/market bnf sheets/2D-H-L.xlsx, sheet Assumptions):
//    * Long when a day trades above the higher of the previous two days'
//      highs, filled at that level (a buy-stop) or at the open when it gaps
//      above it. Short is the mirror: below the lower of the previous two
//      lows, at that level or the open.
//    * Disaster stop fixed at entry: the previous day's low (long) or high
//      (short).
//    * Trailing exit: a long leaves on the first later day that trades below
//      the previous day's low, at that level (or the open when it gaps
//      through); a short above the previous day's high. Whichever of the two
//      stops is nearer the price is the one that fills; the exit is "Stop"
//      when that is the fixed one, "Reversal" when it is the trailing one.
//    * Exits are looked for from the day after entry (one day's minimum hold).
//    * Same-day flip: on an exit day, if the opposite breakout also triggers,
//      the opposite side is entered at its level.
//    * From flat, when both sides trigger on one day, the day's close against
//      its open picks the side (close above the open: long).
//    * One position at a time. What is open on the last day is closed at its
//      close ("Open(EOD)").
//    Checked against the workbook's own trade log (app/threshold_main.cpp):
//    on the workbook's trading days, 1,454 of its 1,467 trades enter on the
//    same day and side, and 1,438 also leave on the same day for the same
//    reason; the rest differ where the two data sources' bars differ.
//
// 2. BANKNIFTY / NIFTY RATIO Z-SCORE ("BankNifty vs Nifty -- Relative-Value
//    Short", market bnf sheets/BNFNF/BNFNF.html, section 5):
//    * R = BANKNIFTY close / NIFTY close; z = (R - mean) / sd over the last
//      120 sessions, today included (sample sd).
//    * z > +1: short the ratio (short BANKNIFTY, long NIFTY); z < -1: long
//      it; |z| < 0.25: flat; in between, hold.
//    * Positions are formed at close t and earn t -> t+1: no look-ahead.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace altair::threshold {

struct DayBar {
    std::int64_t day = 0;   ///< days since 1970-01-01
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
};

enum class ExitWhy : std::uint8_t { Stop, Reversal, OpenEod };

[[nodiscard]] inline const char* exit_text(ExitWhy w) noexcept {
    switch (w) {
    case ExitWhy::Stop: return "Stop";
    case ExitWhy::Reversal: return "Reversal";
    case ExitWhy::OpenEod: return "Open(EOD)";
    }
    return "?";
}

struct BreakoutTrade {
    int side = 0;                        ///< +1 long, -1 short
    std::size_t entry_i = 0, exit_i = 0; ///< bar indices
    double entry = 0.0;
    double stop = 0.0;                   ///< the disaster stop, fixed at entry
    double exit = 0.0;
    ExitWhy why = ExitWhy::Reversal;
    bool flip = false;                   ///< entered on the exit day of the opposite trade
    double mfe = 0.0, mae = 0.0;         ///< points, over the days after entry up to the exit

    [[nodiscard]] double points() const noexcept { return side * (exit - entry); }
    [[nodiscard]] std::size_t days() const noexcept { return exit_i - entry_i; }
};

/// The prev-2-day breakout over `b` (oldest first). The first two bars only
/// set levels.
[[nodiscard]] inline std::vector<BreakoutTrade> two_day_breakout(const std::vector<DayBar>& b) {
    std::vector<BreakoutTrade> out;
    BreakoutTrade cur;
    bool open = false;
    const auto enter = [&](std::size_t i, int side, double px, bool flip) {
        cur = BreakoutTrade{};
        cur.side = side;
        cur.entry_i = i;
        cur.entry = px;
        cur.stop = side > 0 ? b[i - 1].low : b[i - 1].high;
        cur.flip = flip;
        open = true;
    };
    const auto close = [&](std::size_t i, double px, ExitWhy why) {
        cur.exit_i = i;
        cur.exit = px;
        cur.why = why;
        for (std::size_t k = cur.entry_i + 1; k <= i; ++k) {
            const double fav = cur.side > 0 ? b[k].high - cur.entry : cur.entry - b[k].low;
            const double adv = cur.side > 0 ? cur.entry - b[k].low : b[k].high - cur.entry;
            cur.mfe = std::max(cur.mfe, fav);
            cur.mae = std::max(cur.mae, adv);
        }
        out.push_back(cur);
        open = false;
    };
    for (std::size_t i = 2; i < b.size(); ++i) {
        const DayBar& d = b[i];
        const double hi2 = std::max(b[i - 1].high, b[i - 2].high);
        const double lo2 = std::min(b[i - 1].low, b[i - 2].low);
        const bool long_trig = d.high > hi2, short_trig = d.low < lo2;
        // RULE 11: not a bound on a value -- these are stop orders' fills. A
        // stop fills at its level, or at the open when the day opens beyond
        // it (the gap fill the workbook states); nothing is cut off.
        const double long_px = d.open > hi2 ? d.open : hi2;
        const double short_px = d.open < lo2 ? d.open : lo2;
        if (open && i > cur.entry_i) {
            const int side = cur.side;
            bool hit = false;
            double px = 0.0;
            ExitWhy why = ExitWhy::Reversal;
            if (side > 0) {
                const double trail = b[i - 1].low, level = std::max(cur.stop, trail);
                if (d.low < level) {
                    hit = true;
                    // RULE 11: the stop's fill, at its level or at an open beyond it (a gap).
                    px = d.open < level ? d.open : level;
                    why = cur.stop >= trail ? ExitWhy::Stop : ExitWhy::Reversal;
                }
            } else {
                const double trail = b[i - 1].high, level = std::min(cur.stop, trail);
                if (d.high > level) {
                    hit = true;
                    px = d.open > level ? d.open : level;
                    why = cur.stop <= trail ? ExitWhy::Stop : ExitWhy::Reversal;
                }
            }
            if (hit) {
                close(i, px, why);
                if (side > 0 && short_trig) enter(i, -1, short_px, true);
                else if (side < 0 && long_trig) enter(i, +1, long_px, true);
                continue;
            }
        }
        if (!open) {
            if (long_trig && short_trig) {
                if (d.close > d.open) enter(i, +1, long_px, false);
                else enter(i, -1, short_px, false);
            } else if (long_trig) {
                enter(i, +1, long_px, false);
            } else if (short_trig) {
                enter(i, -1, short_px, false);
            }
        }
    }
    if (open && !b.empty()) close(b.size() - 1, b.back().close, ExitWhy::OpenEod);
    return out;
}

struct RatioConfig {
    std::size_t window = 120;   ///< sessions in the mean and sd
    double entry_z = 1.0;       ///< |z| beyond this opens (fading it)
    double flat_z = 0.25;       ///< |z| inside this closes
};

struct RatioDay {
    double ratio = 0.0;
    double z = std::numeric_limits<double>::quiet_NaN();   ///< NaN before a full window
    int pos = 0;   ///< held from this close: +1 long the ratio (long A, short B), -1 short it
};

/// The ratio a[i] / b[i] (same sessions, oldest first), its rolling z and
/// the position held from each close.
[[nodiscard]] inline std::vector<RatioDay> ratio_z(const std::vector<double>& a, const std::vector<double>& b,
                                                   const RatioConfig& cfg = {}) {
    const std::size_t n = std::min(a.size(), b.size());
    std::vector<RatioDay> out(n);
    const std::size_t w = std::max<std::size_t>(cfg.window, 2);
    int pos = 0;
    for (std::size_t i = 0; i < n; ++i) {
        out[i].ratio = b[i] != 0.0 ? a[i] / b[i] : std::numeric_limits<double>::quiet_NaN();
        if (i + 1 >= w) {
            double mean = 0.0;
            for (std::size_t k = i + 1 - w; k <= i; ++k) mean += out[k].ratio;
            mean /= static_cast<double>(w);
            double ss = 0.0;
            for (std::size_t k = i + 1 - w; k <= i; ++k) ss += (out[k].ratio - mean) * (out[k].ratio - mean);
            const double sd = std::sqrt(ss / static_cast<double>(w - 1));
            if (sd > 0.0 && std::isfinite(sd)) {
                const double z = (out[i].ratio - mean) / sd;
                out[i].z = z;
                if (z > cfg.entry_z) pos = -1;
                else if (z < -cfg.entry_z) pos = +1;
                else if (std::fabs(z) < cfg.flat_z) pos = 0;
            }
        }
        out[i].pos = pos;
    }
    return out;
}

/// One holding of the ratio: entered at the close of `entry_i`, left at the
/// close of `exit_i` (the last session when it is still held at the end).
struct RatioTrip {
    int side = 0;
    std::size_t entry_i = 0, exit_i = 0;
    bool open_at_end = false;
};

[[nodiscard]] inline std::vector<RatioTrip> ratio_trips(const std::vector<RatioDay>& d) {
    std::vector<RatioTrip> out;
    RatioTrip cur;
    bool open = false;
    for (std::size_t i = 0; i < d.size(); ++i) {
        const int prev = i > 0 ? d[i - 1].pos : 0;
        if (d[i].pos == prev) continue;
        if (open) {
            cur.exit_i = i;
            out.push_back(cur);
            open = false;
        }
        if (d[i].pos != 0) {
            cur = RatioTrip{d[i].pos, i, i, false};
            open = true;
        }
    }
    if (open && !d.empty()) {
        cur.exit_i = d.size() - 1;
        cur.open_at_end = true;
        out.push_back(cur);
    }
    return out;
}

} // namespace altair::threshold
