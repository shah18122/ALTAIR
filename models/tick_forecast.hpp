// models/tick_forecast.hpp -- forecasts made trade by trade, on raw ticks
// (no bars), each walked forward and scored on RMSE / MAE against the plain
// baselines.
//
// Four questions, every one asked only with what was known at that trade:
//
//   ret   the next trade's log return (bp)        RW (0), running mean, AR ridge
//   sign  the direction of the next price change  persistence, reversal, ridge sign
//   gap   seconds until the next trade            last gap, running mean, EWMA, ridge
//   rv    realised vol of the next 60 s / 300 s   last window, running mean,
//         (root of the summed squared tick          time-of-day mean, HAR ridge
//          returns, bp)
//
// The ridges are refitted every `refit` rows on discounted sums of PAST rows
// only, so a forecast never sees its own target. Overfitting is checked the
// way models/vol_forecast.hpp does it: the final fit's in-sample RMSE on the
// last rows against the real out-of-sample RMSE on the same rows.
//
// Scoring streams (tens of millions of ticks): squared and absolute errors are
// summed in blocks of `block` rows; Diebold-Mariano uses the block means (a
// batch-means t, robust to the autocorrelation inside a block) and the
// thirds are thirds of the blocks.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace altair::tick_forecast {

/// One trade: day as YYYYMMDD, seconds since midnight IST, price, quantity.
struct TkTick {
    std::int32_t day = 0;
    std::int32_t sec = 0;
    double px = 0.0, qty = 0.0;
};

/// Tick files: rows "YYYYMMDD,HH:MM:SS,price[,qty[,oi]]" (also YYYY-MM-DD,
/// DD-MM-YYYY, or date and time in one field), no header needed.
/// "YYYYMMDD" or "YYYY-MM-DD" or "DD-MM-YYYY" / "DD/MM/YYYY" -> YYYYMMDD; 0 if neither.
[[nodiscard]] inline int tk_parse_day(std::string_view s) {
    std::string d;
    for (char c : s) if (std::isdigit(static_cast<unsigned char>(c))) d += c;
    if (d.size() != 8) return 0;
    const bool year_first = s.size() >= 4 && std::isdigit(static_cast<unsigned char>(s[0])) && std::isdigit(static_cast<unsigned char>(s[2])) &&
                            (s.size() == 8 || s[4] == '-' || s[4] == '/');
    if (year_first) return std::atoi(d.c_str());
    return std::atoi((d.substr(4, 4) + d.substr(2, 2) + d.substr(0, 2)).c_str());
}

/// Reads one tick file; calls f(tick) for every good row in file order.
template <class F>
inline std::size_t tk_read_file(const std::filesystem::path& p, F&& f) {
    std::ifstream in(p);
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string_view v{line};
        std::array<std::string_view, 5> c{};
        std::size_t k = 0, from = 0;
        for (std::size_t i = 0; i <= v.size() && k < c.size(); ++i) {
            if (i == v.size() || v[i] == ',') { c[k++] = v.substr(from, i - from); from = i + 1; }
        }
        if (k < 3) continue;
        // Date and time may also be one field ("YYYY-MM-DD HH:MM:SS").
        std::string_view date = c[0], time = c[1];
        std::size_t pi = 2;
        if (const auto sp = c[0].find_first_of(" T"); sp != std::string_view::npos) { date = c[0].substr(0, sp); time = c[0].substr(sp + 1); pi = 1; }
        const int day = tk_parse_day(date);
        if (day == 0 || time.size() < 8 || time[2] != ':') continue;
        const int sec = std::atoi(std::string(time.substr(0, 2)).c_str()) * 3600 + std::atoi(std::string(time.substr(3, 2)).c_str()) * 60 +
                        std::atoi(std::string(time.substr(6, 2)).c_str());
        const double px = std::strtod(std::string(c[pi]).c_str(), nullptr);
        const double qty = pi + 1 < k ? std::strtod(std::string(c[pi + 1]).c_str(), nullptr) : 0.0;
        if (!(px > 0.0)) continue;
        f(TkTick{day, sec, px, qty});
        ++n;
    }
    return n;
}

inline constexpr std::int32_t kTkOpen = 9 * 3600 + 15 * 60, kTkClose = 15 * 3600 + 30 * 60;

/// Streaming RMSE / MAE / DM for M models against one actual.
class TkScorer {
public:
    TkScorer(std::vector<std::string> names, std::size_t block = 2000) : names_(std::move(names)), block_(block) {}

    void add(const double* f, double actual) {
        if (cur_n_ == 0) { cur_se_.assign(names_.size(), 0.0); cur_ae_.assign(names_.size(), 0.0); }
        for (std::size_t m = 0; m < names_.size(); ++m) {
            const double e = f[m] - actual;
            cur_se_[m] += e * e;
            cur_ae_[m] += std::fabs(e);
        }
        ++cur_n_;
        if (cur_n_ == block_) flush();
    }
    void finish() { if (cur_n_ > 0) flush(); }

    [[nodiscard]] const std::vector<std::string>& names() const { return names_; }
    [[nodiscard]] std::size_t n() const {
        std::size_t s = 0;
        for (auto k : bn_) s += k;
        return s;
    }
    [[nodiscard]] double rmse(std::size_t m, std::size_t from = 0, std::size_t to = SIZE_MAX) const {
        double se = 0.0;
        std::size_t k = 0;
        to = std::min(to, bn_.size());
        for (std::size_t b = from; b < to; ++b) { se += bse_[b][m]; k += bn_[b]; }
        return k ? std::sqrt(se / static_cast<double>(k)) : 0.0;
    }
    [[nodiscard]] double mae(std::size_t m) const {
        double ae = 0.0;
        for (const auto& b : bae_) ae += b[m];
        const std::size_t k = n();
        return k ? ae / static_cast<double>(k) : 0.0;
    }
    /// 1 - RMSE(m) / RMSE(base), over blocks [from, to).
    [[nodiscard]] double skill(std::size_t m, std::size_t base, std::size_t from = 0, std::size_t to = SIZE_MAX) const {
        const double b = rmse(base, from, to);
        return b > 0.0 ? 1.0 - rmse(m, from, to) / b : 0.0;
    }
    [[nodiscard]] std::array<double, 3> thirds(std::size_t m, std::size_t base) const {
        const std::size_t B = bn_.size();
        return {skill(m, base, 0, B / 3), skill(m, base, B / 3, 2 * B / 3), skill(m, base, 2 * B / 3, B)};
    }
    /// Batch-means Diebold-Mariano t on squared error, m against base:
    /// negative = m has the smaller error.
    [[nodiscard]] double dm(std::size_t m, std::size_t base) const {
        const std::size_t B = bn_.size();
        if (B < 10) return 0.0;
        std::vector<double> d(B);
        double mean = 0.0;
        for (std::size_t b = 0; b < B; ++b) {
            d[b] = (bse_[b][m] - bse_[b][base]) / static_cast<double>(bn_[b]);
            mean += d[b];
        }
        mean /= static_cast<double>(B);
        double v = 0.0;
        for (double x : d) v += (x - mean) * (x - mean);
        v /= static_cast<double>(B - 1);
        return v > 0.0 ? mean / std::sqrt(v / static_cast<double>(B)) : 0.0;
    }

private:
    void flush() {
        bse_.push_back(cur_se_);
        bae_.push_back(cur_ae_);
        bn_.push_back(cur_n_);
        cur_n_ = 0;
    }
    std::vector<std::string> names_;
    std::size_t block_;
    std::vector<std::vector<double>> bse_, bae_;
    std::vector<std::size_t> bn_;
    std::vector<double> cur_se_, cur_ae_;
    std::size_t cur_n_ = 0;
};

/// Discounted ridge, refitted every `refit` updates from past rows only. It
/// keeps the last `keep` rows (with their out-of-sample forecasts) for the
/// in-sample / out-of-sample check.
template <std::size_t K>
class TkRidge {
public:
    using X = std::array<double, K>;
    explicit TkRidge(double half_life_rows = 2e6, std::size_t refit = 5000, double ridge = 1e-6, std::size_t keep = 20000)
        : lambda_(std::exp(std::log(0.5) / half_life_rows)), refit_(refit), ridge_(ridge), keep_(keep) {}

    [[nodiscard]] double predict(const X& x) const {
        double s = 0.0;
        for (std::size_t i = 0; i < K; ++i) s += w_[i] * x[i];
        return s;
    }
    [[nodiscard]] bool ready() const { return fitted_; }

    /// Learn from a row whose target is now known. `oos` is the forecast the
    /// model made for it before it was known.
    void update(const X& x, double y, double oos) {
        for (std::size_t i = 0; i < K; ++i) {
            for (std::size_t j = 0; j < K; ++j) a_[i][j] = lambda_ * a_[i][j] + x[i] * x[j];
            b_[i] = lambda_ * b_[i] + x[i] * y;
        }
        if (fitted_) {
            tail_.push_back({x, y, oos});
            if (tail_.size() > keep_) tail_.pop_front();
        }
        if (++since_ >= refit_) { since_ = 0; fit(); }
    }

    /// RMSE of the last kept rows: the final weights (in sample) and the
    /// forecasts actually made (out of sample).
    void overfit_check(double& in_rmse, double& oos_rmse) const {
        double si = 0.0, so = 0.0;
        for (const auto& r : tail_) {
            const double ei = predict(r.x) - r.y, eo = r.oos - r.y;
            si += ei * ei;
            so += eo * eo;
        }
        const double n = static_cast<double>(std::max<std::size_t>(1, tail_.size()));
        in_rmse = std::sqrt(si / n);
        oos_rmse = std::sqrt(so / n);
    }
    [[nodiscard]] const X& weights() const { return w_; }

private:
    void fit() {
        auto a = a_;
        auto b = b_;
        double tr = 0.0;
        for (std::size_t i = 0; i < K; ++i) tr += a[i][i];
        for (std::size_t i = 1; i < K; ++i) a[i][i] += ridge_ * tr / static_cast<double>(K);   // never the intercept
        // Gaussian elimination with partial pivoting.
        for (std::size_t c = 0; c < K; ++c) {
            std::size_t p = c;
            for (std::size_t r = c + 1; r < K; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
            if (std::fabs(a[p][c]) < 1e-12) return;
            std::swap(a[p], a[c]);
            std::swap(b[p], b[c]);
            for (std::size_t r = c + 1; r < K; ++r) {
                const double f = a[r][c] / a[c][c];
                for (std::size_t k = c; k < K; ++k) a[r][k] -= f * a[c][k];
                b[r] -= f * b[c];
            }
        }
        X w{};
        for (std::size_t c = K; c-- > 0;) {
            double s = b[c];
            for (std::size_t k = c + 1; k < K; ++k) s -= a[c][k] * w[k];
            w[c] = s / a[c][c];
        }
        for (double v : w) if (!std::isfinite(v)) return;
        w_ = w;
        fitted_ = true;
    }

    struct Kept { X x; double y, oos; };
    double lambda_;
    std::size_t refit_, since_ = 0;
    double ridge_;
    std::size_t keep_;
    std::array<std::array<double, K>, K> a_{};
    X b_{}, w_{};
    bool fitted_ = false;
    std::deque<Kept> tail_;
};

/// Accuracy of a direction call, for the `sign` task.
struct TkHits {
    std::size_t n = 0, hit = 0;
    void add(bool ok) { ++n; hit += ok ? 1 : 0; }
    [[nodiscard]] double rate() const { return n ? static_cast<double>(hit) / static_cast<double>(n) : 0.0; }
    /// z of the hit rate against a coin.
    [[nodiscard]] double z() const {
        return n ? (static_cast<double>(hit) - 0.5 * static_cast<double>(n)) / std::sqrt(0.25 * static_cast<double>(n)) : 0.0;
    }
};

/// Every task over one ordered stream of trades (one underlying). Feed the
/// ticks in time order with `add`; call `finish` at the end.
class TkEngine {
public:
    // ret
    enum : std::size_t { kRetRw, kRetMean, kRetAr, kRetModels };
    // gap
    enum : std::size_t { kGapLast, kGapMean, kGapEwma, kGapRidge, kGapModels };
    // rv
    enum : std::size_t { kRvLast, kRvMean, kRvSeas, kRvHar, kRvModels };
    static constexpr std::array<std::int32_t, 2> kRvH{60, 300};

    TkEngine()
        : ret_({"Random walk", "Running mean", "AR ridge"}),
          gap_({"Last gap", "Running mean", "EWMA", "Ridge"}),
          rv_{TkScorer({"Last window", "Running mean", "Time of day", "HAR ridge"}, 200),
              TkScorer({"Last window", "Running mean", "Time of day", "HAR ridge"}, 50)} {}

    void add(const TkTick& t) {
        if (!(t.px > 0.0) || t.sec < kTkOpen || t.sec >= kTkClose) return;
        ++ticks_;
        if (t.day != day_) new_day(t);

        // ---- the row made at the previous trade now has its target ----
        const double r = have_last_ ? 1e4 * std::log(t.px / last_px_) : 0.0;
        if (have_last_ && pending_ret_) {
            ret_f_[kRetAr] = ar_.ready() ? ar_.predict(ret_x_) : 0.0;
            ret_.add(ret_f_.data(), r);
            if (r != 0.0 && ret_f_[kRetAr] != 0.0) ar_dir_.add((r > 0) == (ret_f_[kRetAr] > 0));
            ar_.update(ret_x_, r, ret_f_[kRetAr]);
            ret_mean_n_ += 1.0;
            ret_mean_ += (r - ret_mean_) / ret_mean_n_;
        }
        if (have_last_ && pending_gap_) {
            const double g = static_cast<double>(t.sec - last_sec_);
            gap_f_[kGapRidge] = gap_r_.ready() ? std::max(0.0, gap_r_.predict(gap_x_)) : gap_f_[kGapMean];
            gap_.add(gap_f_.data(), g);
            gap_r_.update(gap_x_, g, gap_f_[kGapRidge]);
            gap_mean_n_ += 1.0;
            gap_mean_ += (g - gap_mean_) / gap_mean_n_;
            gap_ewma_ = gap_ewma_ < 0 ? g : 0.95 * gap_ewma_ + 0.05 * g;
            last_gap_ = g;
        }
        // sign: the next non-zero change against the last one
        if (have_last_ && r != 0.0) {
            const int s = r > 0 ? 1 : -1;
            if (last_sign_ != 0) {
                persist_.add(s == last_sign_);
                reverse_.add(s == -last_sign_);
                if (sign_pred_ != 0) sign_ridge_.add(s == sign_pred_);
                sign_r_.update(sign_x_, static_cast<double>(s), static_cast<double>(sign_pred_));
            }
            last_sign_ = s;
        }

        // ---- realised vol windows: close every grid boundary this trade passed ----
        for (std::size_t h = 0; h < kRvH.size(); ++h) advance_rv(h, t.sec, r);

        // ---- state after this trade ----
        flow_.push_back({r == 0.0 ? (flow_.empty() ? 0.0 : flow_.back().s) : (r > 0 ? 1.0 : -1.0), t.qty});
        if (flow_.size() > 20) flow_.pop_front();
        hist_r_[2] = hist_r_[1]; hist_r_[1] = hist_r_[0]; hist_r_[0] = r;
        vwap_pq_ = 0.98 * vwap_pq_ + 0.02 * t.px * std::max(1.0, t.qty);
        vwap_q_ = 0.98 * vwap_q_ + 0.02 * std::max(1.0, t.qty);
        last_px_ = t.px;
        last_sec_ = t.sec;
        have_last_ = true;

        // ---- forecasts for the next trade ----
        double fs = 0.0, fq = 0.0;
        for (const auto& f : flow_) { fs += f.s * f.q; fq += f.q; }
        const double flow = fq > 0 ? fs / fq : 0.0;
        const double dev = vwap_q_ > 0 ? 1e4 * std::log(t.px / (vwap_pq_ / vwap_q_)) : 0.0;
        ret_x_ = {1.0, hist_r_[0], hist_r_[1], hist_r_[2], flow, dev};
        ret_f_[kRetRw] = 0.0;
        ret_f_[kRetMean] = ret_mean_;
        pending_ret_ = true;

        const double tod = static_cast<double>(t.sec - kTkOpen) / static_cast<double>(kTkClose - kTkOpen);
        gap_f_[kGapLast] = last_gap_;
        gap_f_[kGapMean] = gap_mean_;
        gap_f_[kGapEwma] = gap_ewma_ < 0 ? gap_mean_ : gap_ewma_;
        gap_x_ = {1.0, last_gap_, gap_f_[kGapEwma], std::log1p(t.qty), tod, tod * tod};
        pending_gap_ = true;

        sign_x_ = {1.0, static_cast<double>(last_sign_), flow, dev / 10.0};
        const double sp = sign_r_.ready() ? sign_r_.predict(sign_x_) : 0.0;
        sign_pred_ = sp > 0 ? 1 : sp < 0 ? -1 : 0;
    }

    void finish() {
        ret_.finish();
        gap_.finish();
        for (auto& s : rv_) s.finish();
    }

    // ---- results ----
    [[nodiscard]] std::size_t ticks() const { return ticks_; }
    [[nodiscard]] std::size_t days() const { return days_; }
    [[nodiscard]] const TkScorer& ret() const { return ret_; }
    [[nodiscard]] const TkScorer& gap() const { return gap_; }
    [[nodiscard]] const TkScorer& rv(std::size_t h) const { return rv_[h]; }
    [[nodiscard]] const TkHits& ar_direction() const { return ar_dir_; }
    [[nodiscard]] const TkHits& persistence() const { return persist_; }
    [[nodiscard]] const TkHits& reversal() const { return reverse_; }
    [[nodiscard]] const TkHits& sign_ridge() const { return sign_ridge_; }
    void overfit_ret(double& in, double& out) const { ar_.overfit_check(in, out); }
    void overfit_gap(double& in, double& out) const { gap_r_.overfit_check(in, out); }
    void overfit_rv(std::size_t h, double& in, double& out) const { rv_r_[h].overfit_check(in, out); }

private:
    struct Flow { double s, q; };
    struct RvState {
        std::int32_t grid_end = 0;              ///< end of the window being filled
        double cur = 0.0;                       ///< summed r^2 in it
        std::deque<double> past;                ///< completed windows today (vol, bp)
        bool pending = false, partial = false;  ///< partial: the day's first window began before the first trade
        std::array<double, kRvModels> f{};
        std::array<double, 5> x{};
        double mean = 0.0, mean_n = 0.0;
        std::vector<double> seas, seas_n;       ///< running mean per slot of the day
        std::size_t slot = 0;
    };

    void new_day(const TkTick& t) {
        day_ = t.day;
        ++days_;
        have_last_ = pending_ret_ = pending_gap_ = false;   // no overnight return or gap
        last_sign_ = 0;
        sign_pred_ = 0;
        flow_.clear();
        hist_r_ = {};
        vwap_pq_ = vwap_q_ = 0.0;
        for (std::size_t h = 0; h < kRvH.size(); ++h) {
            auto& s = rv_s_[h];
            const std::int32_t H = kRvH[h];
            s.grid_end = kTkOpen + ((t.sec - kTkOpen) / H + 1) * H;
            s.cur = 0.0;
            s.past.clear();
            s.pending = false;
            s.partial = true;
            if (s.seas.empty()) { s.seas.assign(static_cast<std::size_t>((kTkClose - kTkOpen) / H) + 1, 0.0); s.seas_n = s.seas; }
        }
    }

    void advance_rv(std::size_t h, std::int32_t sec, double r) {
        auto& s = rv_s_[h];
        const std::int32_t H = kRvH[h];
        while (sec >= s.grid_end && s.grid_end <= kTkClose) {
            const double vol = std::sqrt(s.cur);
            if (s.pending) {
                s.f[kRvHar] = rv_r_[h].ready() ? std::max(0.0, rv_r_[h].predict(s.x)) : s.f[kRvSeas];
                rv_[h].add(s.f.data(), vol);
                rv_r_[h].update(s.x, vol, s.f[kRvHar]);
            }
            const std::size_t slot = static_cast<std::size_t>((s.grid_end - kTkOpen) / H) - 1;
            if (s.partial) {
                // Not a whole window: learn nothing from it, forecast nothing from it.
                s.partial = false;
                s.cur = 0.0;
                s.pending = false;
                s.grid_end += H;
                continue;
            }
            if (slot < s.seas.size()) {
                s.seas_n[slot] += 1.0;
                s.seas[slot] += (vol - s.seas[slot]) / s.seas_n[slot];
            }
            s.mean_n += 1.0;
            s.mean += (vol - s.mean) / s.mean_n;
            s.past.push_back(vol);
            if (s.past.size() > 30) s.past.pop_front();
            s.cur = 0.0;
            // forecast the window that starts now, if it ends inside the session
            const std::int32_t next_end = s.grid_end + H;
            const std::size_t next_slot = static_cast<std::size_t>((next_end - kTkOpen) / H) - 1;
            if (next_end <= kTkClose && s.mean_n > 0) {
                const auto avg_last = [&](std::size_t k) {
                    double a = 0.0;
                    std::size_t n = 0;
                    for (std::size_t i = s.past.size(); i-- > 0 && n < k; ++n) a += s.past[i];
                    return n ? a / static_cast<double>(n) : s.mean;
                };
                const double seas = next_slot < s.seas.size() && s.seas_n[next_slot] > 0 ? s.seas[next_slot] : s.mean;
                s.f[kRvLast] = vol;
                s.f[kRvMean] = s.mean;
                s.f[kRvSeas] = seas;
                s.x = {1.0, vol, avg_last(5), avg_last(30), seas};
                s.pending = true;
            } else {
                s.pending = false;
            }
            s.grid_end = next_end;
        }
        s.cur += r * r;
    }

    TkScorer ret_, gap_;
    std::array<TkScorer, 2> rv_;
    TkRidge<6> ar_{2e6, 5000}, gap_r_{2e6, 5000};
    TkRidge<4> sign_r_{2e6, 5000};
    std::array<TkRidge<5>, 2> rv_r_{TkRidge<5>(2e4, 200, 1e-6, 5000), TkRidge<5>(5e3, 50, 1e-6, 2000)};
    std::array<RvState, 2> rv_s_{};
    TkHits ar_dir_, persist_, reverse_, sign_ridge_;

    std::size_t ticks_ = 0, days_ = 0;
    std::int32_t day_ = 0, last_sec_ = 0;
    double last_px_ = 0.0;
    bool have_last_ = false, pending_ret_ = false, pending_gap_ = false;
    std::array<double, kRetModels> ret_f_{};
    std::array<double, 6> ret_x_{};
    double ret_mean_ = 0.0, ret_mean_n_ = 0.0;
    std::array<double, kGapModels> gap_f_{};
    std::array<double, 6> gap_x_{};
    double gap_mean_ = 0.0, gap_mean_n_ = 0.0, gap_ewma_ = -1.0, last_gap_ = 0.0;
    int last_sign_ = 0, sign_pred_ = 0;
    std::array<double, 4> sign_x_{};
    std::deque<Flow> flow_;
    std::array<double, 3> hist_r_{};
    double vwap_pq_ = 0.0, vwap_q_ = 0.0;
};

} // namespace altair::tick_forecast
