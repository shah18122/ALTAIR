// book/depth_study.hpp -- does the order book predict the next mid-price move?
//
// THE HFT QUESTION, ASKED HONESTLY. Market makers and HFT desks earn from the
// spread and from seconds-ahead order-flow signals. The best documented one is
// ORDER FLOW IMBALANCE (Cont, Kukanov & Stoikov 2014, "The Price Impact of
// Order Book Events"): between two book updates,
//   e = 1{Pb' >= Pb} qb' - 1{Pb' <= Pb} qb - 1{Pa' <= Pa} qa' + 1{Pa' >= Pa} qa
// -- bids added or lifted, asks added or hit -- and over short intervals the
// mid moves linearly with the summed e, with slope ~ 1 / depth. That is a
// CONTEMPORANEOUS fit; whether OFI over the last few seconds predicts the
// NEXT few is a different, much weaker claim, and the one a trader needs.
//
// This file answers both from recorded FYERS depth (altair_fyers_ticker
// --depth --jsonl --stamp), per symbol, on a clock grid:
//   * contemporaneous: d(mid) over (t - step, t] on OFI over the same interval
//     -- the paper's regression, a check that the data behaves;
//   * predictive: d(mid) over (t, t + H] on OFI over (t - lookback, t], the
//     level-1 imbalance and the microprice offset at t. Fitted on the first
//     half of the recording, scored on the second (out-of-sample R^2, and how
//     often the sign is right), and set against the spread a trade must cross.
// Grid points whose window straddles a gap in the recording (> `max_gap_ms`,
// e.g. overnight) are skipped.
//
// Research only. Nothing here can execute at HFT latency, and the verdict is
// stated in those terms.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altair {

inline constexpr std::size_t kDepthStudyLevels = 5;

struct DepthQuote {
    std::array<double, kDepthStudyLevels> bid{}, ask{}, bid_q{}, ask_q{};
    [[nodiscard]] bool valid() const noexcept { return bid[0] > 0.0 && ask[0] > bid[0] && bid_q[0] > 0.0 && ask_q[0] > 0.0; }
    [[nodiscard]] double mid() const noexcept { return 0.5 * (bid[0] + ask[0]); }
    [[nodiscard]] double spread() const noexcept { return ask[0] - bid[0]; }
};

/// Cont-Kukanov-Stoikov order flow imbalance at one level between two books.
[[nodiscard]] inline double ofi_level(const DepthQuote& prev, const DepthQuote& cur, std::size_t k) noexcept {
    double e = 0.0;
    if (cur.bid[k] >= prev.bid[k]) { e += cur.bid_q[k]; }
    if (cur.bid[k] <= prev.bid[k]) { e -= prev.bid_q[k]; }
    if (cur.ask[k] <= prev.ask[k]) { e -= cur.ask_q[k]; }
    if (cur.ask[k] >= prev.ask[k]) { e += prev.ask_q[k]; }
    return e;
}

/// OFI summed over the first `levels` levels (multi-level OFI). More levels
/// than the book holds is refused (NaN), not quietly summed over fewer.
[[nodiscard]] inline double ofi(const DepthQuote& prev, const DepthQuote& cur, std::size_t levels = 1) noexcept {
    if (levels == 0 || levels > kDepthStudyLevels) { return std::numeric_limits<double>::quiet_NaN(); }
    double e = 0.0;
    for (std::size_t k = 0; k < levels; ++k) { e += ofi_level(prev, cur, k); }
    return e;
}

/// (bid size - ask size) / (bid size + ask size) at level 1, in [-1, 1].
[[nodiscard]] inline double l1_imbalance(const DepthQuote& q) noexcept {
    const double s = q.bid_q[0] + q.ask_q[0];
    return s > 0.0 ? (q.bid_q[0] - q.ask_q[0]) / s : 0.0;
}

/// Microprice minus mid, as a fraction of the spread: size-weighted toward the thinner side.
[[nodiscard]] inline double microprice_offset(const DepthQuote& q) noexcept {
    const double s = q.bid_q[0] + q.ask_q[0];
    if (!(s > 0.0) || !(q.spread() > 0.0)) { return 0.0; }
    const double micro = (q.ask[0] * q.bid_q[0] + q.bid[0] * q.ask_q[0]) / s;
    return (micro - q.mid()) / q.spread();
}

struct DepthEvent {
    std::int64_t t_ms = 0;
    DepthQuote q;
    double ofi1 = 0.0, ofi5 = 0.0;   ///< since the previous event of this symbol
};

/// Parse recorded JSONL (one SDK on_message object per line, with "recv_ms"
/// from --stamp) into per-symbol event series. Depth messages ("type":"dp")
/// update a running book: a field absent from a message keeps its last
/// value. Lines without recv_ms, or not depth, are counted and skipped.
struct DepthParseReport { std::size_t lines = 0, depth = 0, unstamped = 0, other = 0, invalid = 0; };

namespace depth_detail {

[[nodiscard]] inline std::optional<double> field(std::string_view line, std::string_view name) {
    std::string key;
    key.reserve(name.size() + 3);
    key += '"';
    key += name;
    key += "\":";
    const auto at = line.find(key);
    if (at == std::string_view::npos) { return std::nullopt; }
    std::size_t i = at + key.size();
    const std::size_t start = i;
    while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.' || line[i] == '-'
                               || line[i] == 'e' || line[i] == 'E' || line[i] == '+')) { ++i; }
    if (i == start) { return std::nullopt; }
    const std::string num{line.substr(start, i - start)};
    char* end = nullptr;
    const double v = std::strtod(num.c_str(), &end);
    if (end == num.c_str() || !std::isfinite(v)) { return std::nullopt; }
    return v;
}

[[nodiscard]] inline std::optional<std::string> text(std::string_view line, std::string_view name) {
    std::string key = "\"" + std::string{name} + "\":\"";
    const auto at = line.find(key);
    if (at == std::string_view::npos) { return std::nullopt; }
    const auto end = line.find('"', at + key.size());
    if (end == std::string_view::npos) { return std::nullopt; }
    return std::string{line.substr(at + key.size(), end - at - key.size())};
}

} // namespace depth_detail

class DepthRecorderReader {
public:
    void line(std::string_view l) {
        ++rep_.lines;
        const auto type = depth_detail::text(l, "type");
        if (!type || *type != "dp") { ++rep_.other; return; }
        const auto t = depth_detail::field(l, "recv_ms");
        if (!t) { ++rep_.unstamped; return; }
        const auto sym = depth_detail::text(l, "symbol");
        if (!sym) { ++rep_.invalid; return; }
        State& s = book_[*sym];
        DepthQuote q = s.q;
        static constexpr std::array<std::string_view, 5> kb{"bid_price1", "bid_price2", "bid_price3", "bid_price4", "bid_price5"};
        static constexpr std::array<std::string_view, 5> ka{"ask_price1", "ask_price2", "ask_price3", "ask_price4", "ask_price5"};
        static constexpr std::array<std::string_view, 5> kbq{"bid_size1", "bid_size2", "bid_size3", "bid_size4", "bid_size5"};
        static constexpr std::array<std::string_view, 5> kaq{"ask_size1", "ask_size2", "ask_size3", "ask_size4", "ask_size5"};
        for (std::size_t k = 0; k < kDepthStudyLevels; ++k) {
            if (const auto v = depth_detail::field(l, kb[k])) { q.bid[k] = *v; }
            if (const auto v = depth_detail::field(l, ka[k])) { q.ask[k] = *v; }
            if (const auto v = depth_detail::field(l, kbq[k])) { q.bid_q[k] = *v; }
            if (const auto v = depth_detail::field(l, kaq[k])) { q.ask_q[k] = *v; }
        }
        s.q = q;
        if (!q.valid()) { ++rep_.invalid; return; }
        ++rep_.depth;
        DepthEvent e;
        e.t_ms = static_cast<std::int64_t>(*t);
        e.q = q;
        if (s.have) {
            e.ofi1 = ofi(s.last_valid, q, 1);
            e.ofi5 = ofi(s.last_valid, q, kDepthStudyLevels);
        }
        s.last_valid = q;
        s.have = true;
        events_[*sym].push_back(e);
    }
    [[nodiscard]] const std::map<std::string, std::vector<DepthEvent>>& events() const noexcept { return events_; }
    [[nodiscard]] const DepthParseReport& report() const noexcept { return rep_; }

private:
    struct State { DepthQuote q, last_valid; bool have = false; };
    std::map<std::string, State> book_;
    std::map<std::string, std::vector<DepthEvent>> events_;
    DepthParseReport rep_;
};

struct DepthStudyPolicy {
    std::int64_t step_ms = 1000;
    std::int64_t lookback_ms = 5000;
    std::int64_t horizon_ms = 5000;
    std::int64_t max_gap_ms = 60'000;
};

struct DepthStudyResult {
    std::size_t samples = 0, train = 0, test = 0;
    double contemp_beta = 0.0, contemp_r2 = 0.0;   ///< d(mid, bp) on OFI5 over the same step, all samples
    double oos_r2_ofi = 0.0, oos_r2_all = 0.0;     ///< predictive, out of sample
    double hit_rate = 0.0;                          ///< sign right, test samples where both are non-zero
    double mean_abs_move_bp = 0.0, mean_spread_bp = 0.0, mean_abs_pred_bp = 0.0;
    double share_pred_beyond_half_spread = 0.0;    ///< test samples whose |prediction| exceeds half the spread
};

namespace depth_detail {

/// OLS of y on [1, X] by normal equations (k <= 4); returns false if singular.
inline bool ols(const std::vector<std::array<double, 4>>& x, const std::vector<double>& y, std::size_t k,
                std::array<double, 5>& beta) {
    const std::size_t p = k + 1;
    double m[5][5] = {}, v[5] = {};
    for (std::size_t i = 0; i < y.size(); ++i) {
        double row[5] = {1.0, 0, 0, 0, 0};
        for (std::size_t j = 0; j < k; ++j) { row[j + 1] = x[i][j]; }
        for (std::size_t a = 0; a < p; ++a) {
            for (std::size_t b = 0; b < p; ++b) { m[a][b] += row[a] * row[b]; }
            v[a] += row[a] * y[i];
        }
    }
    for (std::size_t c = 0; c < p; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < p; ++r) { if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) { piv = r; } }
        if (!(std::fabs(m[piv][c]) > 1e-12)) { return false; }
        for (std::size_t q = 0; q < p; ++q) { std::swap(m[c][q], m[piv][q]); }
        std::swap(v[c], v[piv]);
        for (std::size_t r = 0; r < p; ++r) {
            if (r == c) { continue; }
            const double f = m[r][c] / m[c][c];
            for (std::size_t q = c; q < p; ++q) { m[r][q] -= f * m[c][q]; }
            v[r] -= f * v[c];
        }
    }
    beta = {};
    for (std::size_t c = 0; c < p; ++c) { beta[c] = v[c] / m[c][c]; }
    return true;
}

inline double predict(const std::array<double, 5>& b, const std::array<double, 4>& x, std::size_t k) {
    double s = b[0];
    for (std::size_t j = 0; j < k; ++j) { s += b[j + 1] * x[j]; }
    return s;
}

} // namespace depth_detail

/// Run the study on one symbol's events (sorted by time).
[[nodiscard]] inline std::optional<DepthStudyResult> depth_study(const std::vector<DepthEvent>& ev, const DepthStudyPolicy& p) {
    if (ev.size() < 100 || p.step_ms <= 0 || p.lookback_ms <= 0 || p.horizon_ms <= 0) { return std::nullopt; }
    // Cumulative OFI and the book in force at any time: binary search on event times.
    std::vector<std::int64_t> t(ev.size());
    std::vector<double> c1(ev.size()), c5(ev.size());
    double a1 = 0, a5 = 0;
    for (std::size_t i = 0; i < ev.size(); ++i) { t[i] = ev[i].t_ms; a1 += ev[i].ofi1; a5 += ev[i].ofi5; c1[i] = a1; c5[i] = a5; }
    const auto at = [&](std::int64_t when) -> std::ptrdiff_t {   // last event at or before `when`, -1 if none
        return std::upper_bound(t.begin(), t.end(), when) - t.begin() - 1;
    };
    // A point is usable when no gap > max_gap lies in [from, to].
    std::vector<std::int64_t> gap_after;   // event times followed by a long gap
    for (std::size_t i = 0; i + 1 < t.size(); ++i) { if (t[i + 1] - t[i] > p.max_gap_ms) { gap_after.push_back(t[i]); } }
    const auto clean = [&](std::int64_t from, std::int64_t to) {
        const auto g = std::lower_bound(gap_after.begin(), gap_after.end(), from);
        return g == gap_after.end() || *g >= to;
    };

    struct Row { std::array<double, 4> x; double y_pred, y_now, ofi_now, spread_bp; };
    std::vector<Row> rows;
    for (std::int64_t g = t.front() + p.lookback_ms; g + p.horizon_ms <= t.back(); g += p.step_ms) {
        if (!clean(g - p.lookback_ms, g + p.horizon_ms)) { continue; }
        const auto i = at(g), j = at(g + p.horizon_ms), l = at(g - p.lookback_ms), s = at(g - p.step_ms);
        if (i < 0 || j < 0 || l < 0 || s < 0) { continue; }
        const DepthQuote& q = ev[static_cast<std::size_t>(i)].q;
        const double mid = q.mid();
        const double ofi_back = c5[static_cast<std::size_t>(i)] - c5[static_cast<std::size_t>(l)];
        const double ofi1_back = c1[static_cast<std::size_t>(i)] - c1[static_cast<std::size_t>(l)];
        Row r;
        r.x = {ofi_back, ofi1_back, l1_imbalance(q), microprice_offset(q)};
        r.y_pred = 1e4 * (ev[static_cast<std::size_t>(j)].q.mid() / mid - 1.0);
        r.y_now = 1e4 * (mid / ev[static_cast<std::size_t>(s)].q.mid() - 1.0);
        r.ofi_now = c5[static_cast<std::size_t>(i)] - c5[static_cast<std::size_t>(s)];
        r.spread_bp = 1e4 * q.spread() / mid;
        rows.push_back(r);
    }
    if (rows.size() < 60) { return std::nullopt; }
    DepthStudyResult out;
    out.samples = rows.size();
    // Contemporaneous: all samples.
    {
        std::vector<std::array<double, 4>> x;
        std::vector<double> y;
        for (const auto& r : rows) { x.push_back({r.ofi_now, 0, 0, 0}); y.push_back(r.y_now); }
        std::array<double, 5> b{};
        if (depth_detail::ols(x, y, 1, b)) {
            double my = 0, ss = 0, sr = 0;
            for (const double v : y) { my += v; }
            my /= static_cast<double>(y.size());
            for (std::size_t i = 0; i < y.size(); ++i) {
                const double e = y[i] - depth_detail::predict(b, x[i], 1);
                sr += e * e;
                ss += (y[i] - my) * (y[i] - my);
            }
            out.contemp_beta = b[1];
            out.contemp_r2 = ss > 0 ? 1.0 - sr / ss : 0.0;
        }
    }
    // Predictive: fit on the first half, score on the second.
    const std::size_t half = rows.size() / 2;
    out.train = half;
    out.test = rows.size() - half;
    // The full model keeps only the regressors that vary in the training half:
    // a constant column (a book whose sizes never change) is not information.
    std::vector<std::size_t> cols;
    for (std::size_t j = 0; j < 4; ++j) {
        double lo = rows[0].x[j], hi = rows[0].x[j];
        for (std::size_t i = 0; i < half; ++i) { lo = std::min(lo, rows[i].x[j]); hi = std::max(hi, rows[i].x[j]); }
        if (hi > lo) { cols.push_back(j); }
    }
    const auto pick = [&](const std::array<double, 4>& full, bool all) {
        std::array<double, 4> v{};
        if (!all) { v[0] = full[0]; return v; }
        for (std::size_t j = 0; j < cols.size(); ++j) { v[j] = full[cols[j]]; }
        return v;
    };
    for (const bool all : {false, true}) {
        std::array<double, 5> b{};
        std::size_t k = 0;
        std::vector<double> y;
        for (std::size_t i = 0; i < half; ++i) { y.push_back(rows[i].y_pred); }
        // Collinear columns (level-1 and 5-level OFI can coincide) make the fit
        // singular: drop from the end until it solves.
        bool solved = false;
        while (!solved) {
            k = all ? cols.size() : 1;
            if (k == 0) { break; }
            std::vector<std::array<double, 4>> x;
            for (std::size_t i = 0; i < half; ++i) { x.push_back(pick(rows[i].x, all)); }
            solved = depth_detail::ols(x, y, k, b);
            if (!solved) {
                if (!all) { break; }
                cols.pop_back();
            }
        }
        if (!solved) { continue; }
        double my = 0;
        for (const double v : y) { my += v; }
        my /= static_cast<double>(y.size());   // the training mean: an honest out-of-sample baseline
        double sr = 0, ss = 0, hits = 0, n_hit = 0, absm = 0, absp = 0, spr = 0, beyond = 0;
        for (std::size_t i = half; i < rows.size(); ++i) {
            const double pr = depth_detail::predict(b, pick(rows[i].x, all), k);
            const double yv = rows[i].y_pred;
            sr += (yv - pr) * (yv - pr);
            ss += (yv - my) * (yv - my);
            if (yv != 0.0 && pr != 0.0) { n_hit += 1; hits += (yv > 0) == (pr > 0) ? 1 : 0; }
            absm += std::fabs(yv);
            absp += std::fabs(pr);
            spr += rows[i].spread_bp;
            beyond += std::fabs(pr) > 0.5 * rows[i].spread_bp ? 1 : 0;
        }
        const double n = static_cast<double>(rows.size() - half);
        const double r2 = ss > 0 ? 1.0 - sr / ss : 0.0;
        if (!all) { out.oos_r2_ofi = r2; continue; }
        out.oos_r2_all = r2;
        out.hit_rate = n_hit > 0 ? hits / n_hit : 0.0;
        out.mean_abs_move_bp = absm / n;
        out.mean_abs_pred_bp = absp / n;
        out.mean_spread_bp = spr / n;
        out.share_pred_beyond_half_spread = beyond / n;
    }
    return out;
}

} // namespace altair
