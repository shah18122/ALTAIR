// live/basket_alpha.hpp -- BANKNIFTY future vs its bank stocks: a forecast fitted on Drive ticks,
// run tick by tick on the FYERS feed (live or simulation), paper only.
//
// THE MODEL. research/signals/fit.py fits, on Smit's Drive ticks (2020, 2022, 2023; source
// drive-ticks, not FYERS), a linear forecast of the future's log return over the next 10 s and
// 60 s (starting 1 s after the signal) on eight features, every one known at that second:
//   rev10, rev60    the future's own last 10 s / 60 s return, bp
//   misprice        -(gap - EWMA(gap)) bp; gap = log F - sum_j beta_j log P_j (the bank basket)
//   leadgap10/30    basket return minus future return over the last 10 s / 30 s, bp
//   breadth         sum_j |beta_j| sign(log P_j(t) - log P_j(t-60)) / sum_j |beta_j|
//   flow60/300      tick-rule signed future volume / volume, last 60 s / 300 s
// The coefficients and betas are config/signals/basket_bnf.csv. research/signals/README.md has
// the walk-forward results: real but small (~1 bp at 10 s), below a round trip's cost.
//
// THE GRID. Exactly as the fit: one slot a second (feed clock), last print price carried forward,
// volumes summed in the slot. A second is closed when the first event of a later second arrives.
// No forecast until 300 s of history and every stock has printed.
//
// THE TRADE (paper). Every 5 s from 09:20 to 15:15: when |forecast 60 s| exceeds the round trip's
// expense in bp plus a safety margin, open one lot of the near future that way; close after 60 s
// or at the loss limit. Every forecast goes to <live dir>/basket_alpha/forecasts_<day>.csv, so a
// simulated or recorded session can be scored (research/signals/score_live.py); every entry and
// exit, with its reason, to <live dir>/basket_alpha/trades.csv.
#pragma once

#include <live/bars.hpp>
#include <live/engine.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace altair::live {

inline constexpr std::size_t kBasketFeatures = 8;
inline constexpr std::array<const char*, kBasketFeatures> kBasketFeatureNames{
    "rev10", "rev60", "misprice", "leadgap10", "leadgap30", "breadth", "flow60", "flow300"};

struct BasketAlphaParams {
    double ew_half_s = 1800.0;
    std::vector<std::pair<std::string, double>> betas;   ///< stock symbol, beta
    std::array<double, 2> c0{};                          ///< [10 s, 60 s]
    std::array<std::array<double, kBasketFeatures>, 2> coef{};
    bool ok = false;
    std::string why;
};

/// Reads config/signals/basket_bnf.csv ("key,value"; '#' comments).
[[nodiscard]] inline BasketAlphaParams read_basket_alpha(const std::string& path) {
    BasketAlphaParams p;
    std::ifstream in(path);
    if (!in) { p.why = "no " + path; return p; }
    std::string line;
    std::array<std::array<bool, kBasketFeatures>, 2> seen{};
    std::array<bool, 2> seen_c0{};
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line == "key,value") continue;
        const auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        const std::string key = line.substr(0, comma);
        char* end = nullptr;
        const double v = std::strtod(line.c_str() + comma + 1, &end);
        if (end == line.c_str() + comma + 1 || !std::isfinite(v)) { p.why = "bad value for " + key; return p; }
        if (key == "ew_half_s") p.ew_half_s = v;
        else if (key.rfind("beta_", 0) == 0) p.betas.emplace_back(key.substr(5), v);
        else if (key == "c0_10" || key == "c0_60") { const int h = key == "c0_10" ? 0 : 1; p.c0[h] = v; seen_c0[h] = true; }
        else if (key.rfind("coef_10_", 0) == 0 || key.rfind("coef_60_", 0) == 0) {
            const int h = key[5] == '1' ? 0 : 1;
            const std::string f = key.substr(8);
            for (std::size_t k = 0; k < kBasketFeatures; ++k)
                if (f == kBasketFeatureNames[k]) { p.coef[h][k] = v; seen[h][k] = true; }
        }
    }
    for (int h = 0; h < 2; ++h) {
        if (!seen_c0[h]) { p.why = "missing c0"; return p; }
        for (std::size_t k = 0; k < kBasketFeatures; ++k)
            if (!seen[h][k]) { p.why = std::string("missing coef for ") + kBasketFeatureNames[k]; return p; }
    }
    if (p.betas.empty()) { p.why = "no betas"; return p; }
    if (!(p.ew_half_s > 0.0)) { p.why = "ew_half_s must be > 0"; return p; }
    p.ok = true;
    return p;
}

/// The one-second grid and the eight features. Pure: no engine, so tests drive it directly.
class BasketAlphaState {
public:
    static constexpr std::int64_t kRing = 512;   ///< > 300 s, the longest look-back

    explicit BasketAlphaState(std::size_t stocks = 0, std::vector<double> betas = {}, double ew_half_s = 1800.0)
        : betas_(std::move(betas)), ew_a_(1.0 - std::pow(0.5, 1.0 / ew_half_s)) {
        if (betas_.size() != stocks) betas_.assign(stocks, 0.0);
        reset();
    }

    void reset() {
        const std::size_t K = betas_.size();
        cur_sec_ = -1;
        slots_ = 0;
        fut_px_ = std::numeric_limits<double>::quiet_NaN();
        stock_px_.assign(K, std::numeric_limits<double>::quiet_NaN());
        last_print_ = 0.0;
        last_sign_ = 0.0;
        sv_cur_ = vol_cur_ = 0.0;
        cum_sv_ = cum_vol_ = 0.0;
        ew_ = std::numeric_limits<double>::quiet_NaN();
        F_.assign(kRing, std::numeric_limits<double>::quiet_NaN());
        B_ = F_;
        CSV_.assign(kRing, 0.0);
        CV_.assign(kRing, 0.0);
        S_.assign(static_cast<std::size_t>(kRing) * K, std::numeric_limits<double>::quiet_NaN());
        EW_ = F_;
    }

    /// Any event at feed time `sec` (whole seconds): closes every earlier open second first.
    void advance(std::int64_t sec) {
        if (cur_sec_ < 0) { cur_sec_ = sec; return; }
        if (sec <= cur_sec_) return;
        close_slot();
        // empty seconds in between: prices carried, no volume
        const std::int64_t gap = std::min<std::int64_t>(sec - cur_sec_ - 1, kRing);
        for (std::int64_t k = 0; k < gap; ++k) close_slot();
        cur_sec_ = sec;
    }
    void on_future(std::int64_t sec, double price, double qty) {
        if (!(price > 0.0)) return;
        advance(sec);
        if (last_print_ > 0.0 && price != last_print_) last_sign_ = price > last_print_ ? 1.0 : -1.0;
        last_print_ = price;
        fut_px_ = price;
        if (qty > 0.0) { sv_cur_ += last_sign_ * qty; vol_cur_ += qty; }
    }
    void on_stock(std::int64_t sec, std::size_t j, double price) {
        if (!(price > 0.0) || j >= stock_px_.size()) return;
        advance(sec);
        stock_px_[j] = price;
    }

    /// Seconds closed so far today.
    [[nodiscard]] std::int64_t slots() const noexcept { return slots_; }
    /// The second the features describe (the last closed one); -1 before any.
    [[nodiscard]] std::int64_t last_closed_sec() const noexcept { return cur_sec_ < 0 || slots_ == 0 ? -1 : cur_sec_ - 1; }
    [[nodiscard]] double future_price() const noexcept { return std::isfinite(F(0)) ? std::exp(F(0)) : 0.0; }

    /// The features at the last closed second; false while any is unknown.
    bool features(std::array<double, kBasketFeatures>& x) const {
        if (slots_ <= 300) return false;
        const double f0 = F(0), f10 = F(10), f30 = F(30), f60 = F(60);
        const double b0 = B(0), b10 = B(10), b30 = B(30);
        const double ew = EW(0);
        if (!std::isfinite(f0 + f10 + f30 + f60 + b0 + b10 + b30 + ew)) return false;
        x[0] = (f0 - f10) * 1e4;
        x[1] = (f0 - f60) * 1e4;
        x[2] = -((f0 - b0) - ew) * 1e4;
        x[3] = ((b0 - b10) - (f0 - f10)) * 1e4;
        x[4] = ((b0 - b30) - (f0 - f30)) * 1e4;
        double num = 0.0, den = 0.0;
        for (std::size_t j = 0; j < betas_.size(); ++j) {
            const double d = S(0, j) - S(60, j);
            if (!std::isfinite(d)) return false;
            num += std::fabs(betas_[j]) * (d > 0.0 ? 1.0 : (d < 0.0 ? -1.0 : 0.0));
            den += std::fabs(betas_[j]);
        }
        if (!(den > 0.0)) return false;
        x[5] = num / den;
        const double v60 = CV(0) - CV(60), v300 = CV(0) - CV(300);
        if (!(v60 > 0.0) || !(v300 > 0.0)) return false;
        x[6] = (CSV(0) - CSV(60)) / v60;
        x[7] = (CSV(0) - CSV(300)) / v300;
        return true;
    }

private:
    [[nodiscard]] std::size_t at(std::int64_t back) const noexcept {
        return static_cast<std::size_t>(((slots_ - 1 - back) % kRing + kRing) % kRing);
    }
    [[nodiscard]] double F(std::int64_t k) const { return k < slots_ ? F_[at(k)] : std::numeric_limits<double>::quiet_NaN(); }
    [[nodiscard]] double B(std::int64_t k) const { return k < slots_ ? B_[at(k)] : std::numeric_limits<double>::quiet_NaN(); }
    [[nodiscard]] double EW(std::int64_t k) const { return k < slots_ ? EW_[at(k)] : std::numeric_limits<double>::quiet_NaN(); }
    [[nodiscard]] double CSV(std::int64_t k) const { return k < slots_ ? CSV_[at(k)] : std::numeric_limits<double>::quiet_NaN(); }
    [[nodiscard]] double CV(std::int64_t k) const { return k < slots_ ? CV_[at(k)] : std::numeric_limits<double>::quiet_NaN(); }
    [[nodiscard]] double S(std::int64_t k, std::size_t j) const {
        return k < slots_ ? S_[at(k) * betas_.size() + j] : std::numeric_limits<double>::quiet_NaN();
    }

    void close_slot() {
        ++slots_;
        const std::size_t i = at(0);
        const double f = fut_px_ > 0.0 ? std::log(fut_px_) : std::numeric_limits<double>::quiet_NaN();
        double b = 0.0;
        for (std::size_t j = 0; j < betas_.size(); ++j) {
            const double s = stock_px_[j] > 0.0 ? std::log(stock_px_[j]) : std::numeric_limits<double>::quiet_NaN();
            S_[i * betas_.size() + j] = s;
            b += betas_[j] * s;
        }
        F_[i] = f;
        B_[i] = b;
        cum_sv_ += sv_cur_;
        cum_vol_ += vol_cur_;
        CSV_[i] = cum_sv_;
        CV_[i] = cum_vol_;
        sv_cur_ = vol_cur_ = 0.0;
        const double g = f - b;
        if (std::isfinite(g)) ew_ = std::isfinite(ew_) ? ew_ + ew_a_ * (g - ew_) : g;
        EW_[i] = ew_;
    }

    std::vector<double> betas_;
    double ew_a_;
    std::int64_t cur_sec_ = -1, slots_ = 0;
    double fut_px_ = 0.0, last_print_ = 0.0, last_sign_ = 0.0;
    std::vector<double> stock_px_;
    double sv_cur_ = 0.0, vol_cur_ = 0.0, cum_sv_ = 0.0, cum_vol_ = 0.0, ew_ = 0.0;
    std::vector<double> F_, B_, CSV_, CV_, S_, EW_;
};

[[nodiscard]] inline double basket_forecast(const BasketAlphaParams& p, int h, const std::array<double, kBasketFeatures>& x) {
    double y = p.c0[static_cast<std::size_t>(h)];
    for (std::size_t k = 0; k < kBasketFeatures; ++k) y += p.coef[static_cast<std::size_t>(h)][k] * x[k];
    return y;
}

struct BasketAlphaRules {
    std::string under = "BANKNIFTY";
    std::string dir;                      ///< <live dir>/basket_alpha; empty = no files
    double safety_bp = 1.0;               ///< on top of the round trip's expense
    std::int64_t lots = 1;
    int first_entry_minute = 9 * 60 + 20, last_entry_minute = 15 * 60 + 15;
    std::int64_t hold_ns = 60'000'000'000;
    int max_trades_day = 20;
    double max_loss_trade = 2000.0, max_daily_loss = 5000.0;
    int sample_s = 5;
};

class LiveBasketAlphaModel final : public LiveModel {
public:
    LiveBasketAlphaModel(BasketAlphaParams p, BasketAlphaRules r) : p_(std::move(p)), r_(std::move(r)) {}

    [[nodiscard]] std::string name() const override { return "Basket alpha " + r_.under; }
    [[nodiscard]] std::string family() const override { return "Order flow"; }

    void on_new_day(LiveEngine& e) override {
        day_ = e.today();
        fut_ = 0;
        stock_of_.clear();
        std::int64_t best = std::numeric_limits<std::int64_t>::max();
        for (const auto& in : e.universe())
            if (in.kind == LiveKind::Future && in.underlying == r_.under && in.expiry_day >= day_ && in.expiry_day < best) {
                best = in.expiry_day;
                fut_ = in.token;
            }
        std::vector<double> betas;
        std::size_t found = 0;
        for (std::size_t j = 0; j < p_.betas.size(); ++j) {
            betas.push_back(p_.betas[j].second);
            for (const auto& in : e.universe())
                if (in.kind == LiveKind::Equity && in.symbol == p_.betas[j].first && in.fyers.rfind("NSE:", 0) == 0) {
                    stock_of_.emplace_back(in.token, j);
                    ++found;
                    break;
                }
        }
        st_ = BasketAlphaState(betas.size(), betas, p_.ew_half_s);
        complete_ = fut_ != 0 && found == p_.betas.size();
        trades_today_ = 0;
        halted_ = false;
        held_ = 0;
        last_sample_ = -1;
        f_ = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
        note_ = complete_ ? "building 5 min of history"
                          : (fut_ == 0 ? "no " + r_.under + " future in the feed"
                                       : std::to_string(found) + " of " + std::to_string(p_.betas.size()) + " bank stocks in the feed (pull main: config/universe_banknifty.csv)");
    }

    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        if (!complete_) return;
        const std::int64_t sec = ns / 1'000'000'000;
        const double px = static_cast<double>(p.last_paise) / 100.0;
        if (p.token == fut_) st_.on_future(sec, px, static_cast<double>(p.last_qty));
        else {
            bool mine = false;
            for (const auto& [tok, j] : stock_of_)
                if (tok == p.token) { st_.on_stock(sec, j, px); mine = true; break; }
            if (!mine) { st_.advance(sec); }
        }
        step(e, ns);
    }
    void on_minute(LiveEngine& e, int) override {
        if (day_ != e.today()) on_new_day(e);
        if (held_ != 0) manage(e, e.clock_ns());
    }

    [[nodiscard]] LiveModelView view(const LiveEngine&) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = halted_ ? "done today" : (held_ != 0 ? "in position" : "watching");
        v.signal = std::isfinite(f_[1]) ? "next 10 s " + live_fmt::num(f_[0]) + " bp, next 60 s " + live_fmt::num(f_[1]) + " bp" : "no forecast yet";
        v.reason = note_;
        v.fields.emplace_back("Trades today", std::to_string(trades_today_) + " of " + std::to_string(r_.max_trades_day));
        v.fields.emplace_back("Fitted on", "Drive ticks 2020-23");
        return v;
    }

    [[nodiscard]] const BasketAlphaState& state() const noexcept { return st_; }

private:
    void step(LiveEngine& e, std::int64_t ns) {
        const std::int64_t t = st_.last_closed_sec();
        if (t < 0 || t == last_sample_ || t % r_.sample_s != 0) {
            if (held_ != 0) manage(e, ns);
            return;
        }
        last_sample_ = t;
        std::array<double, kBasketFeatures> x{};
        if (!st_.features(x)) return;
        f_ = {basket_forecast(p_, 0, x), basket_forecast(p_, 1, x)};
        const int mod = live_minute_of_day(live_ist_minute_index(t * 1'000'000'000));
        log_forecast(t, x);
        if (held_ != 0) { manage(e, ns); return; }
        if (mod < r_.first_entry_minute || mod >= r_.last_entry_minute) return;
        const LiveInstrument* in = e.instrument(fut_);
        const double px = st_.future_price();
        if (in == nullptr || !(px > 0.0)) return;
        const double qty = static_cast<double>(std::max<std::int64_t>(1, in->lot) * r_.lots);
        const int side = f_[1] > 0.0 ? 1 : -1;
        const double cost_bp = (e.book().expense(*in, side > 0, qty, px, ns) + e.book().expense(*in, side < 0, qty, px, ns)) / (qty * px) * 1e4;
        const double need = cost_bp + r_.safety_bp;
        if (!(std::fabs(f_[1]) > need)) { note_ = "forecast " + live_fmt::num(f_[1]) + " bp, needs " + live_fmt::num(need) + " bp"; return; }
        if (halted_ || e.paused() || e.stale() || trades_today_ >= r_.max_trades_day) { note_ = "signal, but no entries now"; return; }
        char b[320];
        std::snprintf(b, sizeof b, "60 s forecast %+.2f bp > costs %.2f + safety %.2f bp (leadgap10 %+.2f, misprice %+.2f, breadth %+.2f, flow60 %+.2f)",
                      f_[1], cost_bp, r_.safety_bp, x[3], x[2], x[5], x[6]);
        std::string why;
        if (!e.book().open(name(), *in, side, r_.lots, ns, b, false, &why)) { note_ = "entry refused: " + why; return; }
        held_ = fut_;
        entry_ns_ = ns;
        ++trades_today_;
        note_ = b;
        e.note_decision(name(), note_);
        log_trade(ns, "ENTRY", side, px, b);
    }

    void manage(LiveEngine& e, std::int64_t ns) {
        const LivePosition* p = e.book().position(name(), held_);
        if (p == nullptr) { finish(e); return; }
        if (p->state == LivePosState::Closing || !p->filled()) return;
        const double u = e.book().unrealised(*p, ns);
        std::string exit;
        if (std::isfinite(u) && -u >= r_.max_loss_trade) exit = "max loss a trade: Rs " + live_fmt::num(-u, 0);
        else if (ns - entry_ns_ >= r_.hold_ns) exit = "held the forecast's 60 s";
        if (exit.empty()) return;
        (void)e.book().close(name(), held_, ns, exit);
        log_trade(ns, "EXIT", 0, st_.future_price(), exit);
        e.note_decision(name(), "EXIT: " + exit);
        if (e.book().position(name(), held_) == nullptr) finish(e);
    }

    void finish(LiveEngine& e) {
        double day = 0.0;
        for (const auto& t : e.book().trades())
            if (t.model == name() && live_day_of(live_ist_minute_index(t.exit_ns)) == e.today()) day += std::isfinite(t.net) ? t.net : t.gross;
        if (-day >= r_.max_daily_loss) halted_ = true;
        note_ = "today Rs " + live_fmt::num(day, 0);
        held_ = 0;
    }

    void append(const std::filesystem::path& path, const std::string& header, const std::string& row) const {
        if (r_.dir.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const bool fresh = !std::filesystem::exists(path);
        std::ofstream o(path, std::ios::app);
        if (fresh) o << header;
        o << row;
    }
    void log_forecast(std::int64_t sec, const std::array<double, kBasketFeatures>& x) const {
        if (r_.dir.empty()) return;
        char b[512];
        int n = std::snprintf(b, sizeof b, "%lld,%.2f,%.4f,%.4f", static_cast<long long>(sec), st_.future_price(), f_[0], f_[1]);
        for (double v : x) n += std::snprintf(b + n, sizeof b - static_cast<std::size_t>(n), ",%.5f", v);
        std::snprintf(b + n, sizeof b - static_cast<std::size_t>(n), "\n");
        std::string head = "epoch_s,future,f10_bp,f60_bp";
        for (const char* f : kBasketFeatureNames) head += std::string(",") + f;
        append(std::filesystem::path(r_.dir) / ("forecasts_" + std::to_string(day_) + ".csv"), head + "\n", b);
    }
    void log_trade(std::int64_t ns, const char* what, int side, double px, std::string reason) const {
        for (auto& ch : reason) if (ch == ',' || ch == '\n') ch = ';';
        char b[96];
        std::snprintf(b, sizeof b, "%lld,%s,%d,%.2f,%.3f,", static_cast<long long>(ns), what, side, px, f_[1]);
        append(std::filesystem::path(r_.dir) / "trades.csv", "time_ns,event,side,future,f60_bp,reason\n", std::string(b) + reason + "\n");
    }

    BasketAlphaParams p_;
    BasketAlphaRules r_;
    BasketAlphaState st_;
    std::int64_t day_ = -1, last_sample_ = -1, entry_ns_ = 0;
    std::uint32_t fut_ = 0, held_ = 0;
    std::vector<std::pair<std::uint32_t, std::size_t>> stock_of_;
    bool complete_ = false, halted_ = false;
    int trades_today_ = 0;
    std::array<double, 2> f_{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    std::string note_;
};

} // namespace altair::live
