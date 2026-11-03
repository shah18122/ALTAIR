// app/obi_select.hpp -- which NSE F&O stocks the OBI trader should watch
// tomorrow, scored on what FYERS showed today (altair_obi_select).
//
// THE IDEA is kalshimarketmaker's market selector: score every candidate by
// what it traded and how its book looked, keep the top N, and let the traders
// run only there. On NSE the candidates are the stocks a recorded session
// streamed (the NIFTY 50 NSE cash books -- every one an F&O stock -- and the
// futures of config/obi_underlyings.csv),
// read off the session tapes -- FYERS data only, recorded by ALTAIR itself.
//
// PER STOCK, over the session (09:15-15:30, sampled at most once a second):
//   value      rupees traded (the day's volume as it moved, times the price);
//   spread     the median bid-ask spread in ticks;
//   depth      the median of the smaller touch size (bid or ask), in rupees;
//   updates    quotes and books seen (how alive the book is).
// SCORE = 0.5 x pct(value) + 0.3 x pct(depth) + 0.2 x (1 - pct(spread)), each
// pct a rank percentile among the candidates (robust to one huge name). The
// order-book trader wants a market that trades, with size at the touch and a
// spread of a tick or two: where an imbalance can show and be acted on.
//
// THE TARGETS written for a chosen stock are a starting point, in rupees a
// share: target = max(8 ticks, 4 x the median spread), stop = target / 2,
// safety = one tick. altair_tune's target_scale tunes them on the tapes.
//
// DROPPING A STOCK is safe by construction: the OBI traders are built when the
// engine starts and square off intraday at 15:20, so a stock taken out of the
// file has nothing working or held by the next session (the deselect-cleanup
// invariant the Kalshi bot enforces by hand).

#pragma once

#include <app/live_tune.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace altair::obi_select {

namespace fs = std::filesystem;

struct StockDay {
    std::string symbol;                 ///< the NSE symbol (the underlying)
    double tick = 0.05;
    double price = 0.0;                 ///< last mid, rupees
    double value = 0.0;                 ///< rupees traded
    std::size_t updates = 0;
    std::vector<double> spread_ticks;   ///< one sample a second at most
    std::vector<double> depth_rs;       ///< the smaller touch side, rupees
    std::int64_t last_sample_ns = 0;
    std::int64_t last_volume = -1;
};

struct Score {
    std::string symbol;
    double price = 0.0, value = 0.0, spread_ticks = 0.0, depth_rs = 0.0, tick = 0.05;
    std::size_t updates = 0, samples = 0;
    double score = 0.0;
    double target = 0.0, stop = 0.0, safety = 0.0;   ///< rupees a share, for config/obi_underlyings.csv
};

[[nodiscard]] inline double median(std::vector<double> v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

/// A model that only watches: every stock's (cash, or its near future when
/// no cash book is streamed) book and trades.
class Collector final : public live::LiveModel {
public:
    explicit Collector(std::shared_ptr<std::map<std::string, StockDay>> out) : out_(std::move(out)) {}
    [[nodiscard]] std::string name() const override { return "OBI stock selector"; }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    void on_minute(live::LiveEngine&, int) override {}
    [[nodiscard]] live::LiveModelView view(const live::LiveEngine&) const override { return {}; }

    void on_book_tick(live::LiveEngine& e, std::uint32_t token, std::int64_t ns) override { sample(e, token, ns); }
    void on_quote(live::LiveEngine& e, std::uint32_t token) override { sample(e, token, e.clock_ns()); }
    void on_print(live::LiveEngine& e, const PricePayload& p, std::int64_t ns) override {
        StockDay* d = day_of(e, p.token);
        if (d == nullptr || !in_session(ns)) return;
        if (p.has(kPriceHasVolume)) {
            if (d->last_volume >= 0 && p.volume > d->last_volume)
                d->value += static_cast<double>(p.volume - d->last_volume) * static_cast<double>(p.last_paise) / 100.0;
            d->last_volume = p.volume;
        }
        sample(e, p.token, ns);
    }

private:
    [[nodiscard]] static bool in_session(std::int64_t ns) {
        const int m = live::live_minute_of_day(live::live_ist_minute_index(ns));
        return m >= live::kLiveOpenMinute && m < live::kLiveCloseMinute;
    }

    /// The day record for `token` when it is a stock's cash book, or the near
    /// future of a stock no cash book is streamed for; nullptr otherwise.
    StockDay* day_of(const live::LiveEngine& e, std::uint32_t token) {
        const live::LiveInstrument* in = e.instrument(token);
        if (in == nullptr) return nullptr;
        if (in->kind == live::LiveKind::Equity) {
            if (in->fyers.rfind("NSE:", 0) != 0) return nullptr;   // the NSE book only (not a BSE twin)
            const std::string sym = in->underlying.empty() ? in->symbol : in->underlying;
            if (sym == "NIFTY" || sym == "BANKNIFTY") return nullptr;
            StockDay& d = (*out_)[sym];
            d.symbol = sym;
            d.tick = in->tick > 0.0 ? in->tick : 0.05;
            cash_.insert(sym);
            return &d;
        }
        if (in->kind == live::LiveKind::Future && in->underlying != "NIFTY" && in->underlying != "BANKNIFTY"
            && cash_.count(in->underlying) == 0) {
            if (const live::LiveInstrument* f = e.near_future(in->underlying); f == nullptr || f->token != token) return nullptr;
            StockDay& d = (*out_)[in->underlying];
            d.symbol = in->underlying;
            d.tick = in->tick > 0.0 ? in->tick : 0.05;
            return &d;
        }
        return nullptr;
    }

    void sample(const live::LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        if (!in_session(ns)) return;
        StockDay* d = day_of(e, token);
        if (d == nullptr) return;
        ++d->updates;
        if (ns - d->last_sample_ns < 1'000'000'000LL) return;
        const live::LiveState* s = e.state(token);
        if (s == nullptr || s->bid <= 0 || s->ask <= s->bid) return;
        d->last_sample_ns = ns;
        const double bid = static_cast<double>(s->bid) / 100.0, ask = static_cast<double>(s->ask) / 100.0;
        d->price = 0.5 * (bid + ask);
        d->spread_ticks.push_back((ask - bid) / d->tick);
        d->depth_rs.push_back(static_cast<double>(std::min(s->bid_qty, s->ask_qty)) * d->price);
    }

    std::shared_ptr<std::map<std::string, StockDay>> out_;
    std::set<std::string> cash_;
};

/// Rank percentile of each value (0 lowest .. 1 highest; ties share the mean rank).
[[nodiscard]] inline std::vector<double> pct_ranks(const std::vector<double>& v) {
    const std::size_t n = v.size();
    std::vector<double> out(n, 0.5);
    if (n < 2) return out;
    std::vector<std::size_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return v[a] < v[b]; });
    for (std::size_t i = 0; i < n;) {
        std::size_t j = i;
        while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
        const double r = (static_cast<double>(i + j) / 2.0) / static_cast<double>(n - 1);
        for (std::size_t k = i; k <= j; ++k) out[idx[k]] = r;
        i = j + 1;
    }
    return out;
}

/// Every stock with at least `min_samples` book samples, best first.
[[nodiscard]] inline std::vector<Score> score(const std::map<std::string, StockDay>& days, std::size_t min_samples = 300) {
    std::vector<Score> s;
    for (const auto& [sym, d] : days) {
        if (d.spread_ticks.size() < min_samples) continue;
        Score x;
        x.symbol = sym;
        x.price = d.price;
        x.value = d.value;
        x.tick = d.tick;
        x.updates = d.updates;
        x.samples = d.spread_ticks.size();
        x.spread_ticks = median(d.spread_ticks);
        x.depth_rs = median(d.depth_rs);
        s.push_back(x);
    }
    std::vector<double> v, dp, sp;
    for (const auto& x : s) { v.push_back(x.value); dp.push_back(x.depth_rs); sp.push_back(x.spread_ticks); }
    const auto pv = pct_ranks(v), pd = pct_ranks(dp), ps = pct_ranks(sp);
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i].score = 0.5 * pv[i] + 0.3 * pd[i] + 0.2 * (1.0 - ps[i]);
        const double tick = s[i].tick;
        const auto round_tick = [tick](double x) { return std::ceil(x / tick - 1e-9) * tick; };
        s[i].target = round_tick(std::max(8.0 * tick, 4.0 * s[i].spread_ticks * tick));
        s[i].stop = round_tick(s[i].target / 2.0);
        s[i].safety = tick;
    }
    std::sort(s.begin(), s.end(), [](const Score& a, const Score& b) { return a.score > b.score || (a.score == b.score && a.symbol < b.symbol); });
    return s;
}

/// Replay `tapes` (one session's) through the collector.
[[nodiscard]] inline bool collect(const std::vector<fs::path>& tapes, std::map<std::string, StockDay>& out, std::string& err) {
    auto shared = std::make_shared<std::map<std::string, StockDay>>();
    tune::Grid g;
    g.key = "obi_select";
    g.model = "OBI stock selector";
    g.points.emplace_back();
    g.make_all = [shared](const tune::Params&) {
        std::vector<std::unique_ptr<live::LiveModel>> v;
        v.push_back(std::make_unique<Collector>(shared));
        return v;
    };
    const live::LiveCostFn none = [](const live::LiveInstrument&, bool, double, double, std::int64_t) { return 0.0; };
    for (const auto& t : tapes) {
        std::vector<tune::DayRun> runs;
        if (!tune::run_tape_group(t, {&g}, none, runs, err)) return false;
    }
    out = std::move(*shared);
    return true;
}

/// config/obi_underlyings.csv with `chosen`, written in one step.
inline bool write_underlyings(const fs::path& path, const std::vector<Score>& chosen, const std::string& day) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "# config/obi_underlyings.csv -- NSE F&O stocks the order-book imbalance trader\n"
             "# (live/obi_trader.hpp, paper only) runs on besides BANKNIFTY.\n#\n"
             "# Written by altair_obi_select from the FYERS session of " << day << ": the stocks\n"
             "# that traded most, with size at the touch and a tight spread (app/obi_select.hpp).\n"
             "# target, stop and safety are rupees a SHARE: target = max(8 ticks, 4 x the median\n"
             "# spread), stop = half, safety = one tick -- a starting point that altair_tune tunes.\n"
             "# Edit by hand at will; restart the price service and the live engine after an edit.\n"
             "symbol,target,stop,safety\n";
        char b[160];
        for (const auto& s : chosen) {
            std::snprintf(b, sizeof b, "%s,%.2f,%.2f,%.2f\n", s.symbol.c_str(), s.target, s.stop, s.safety);
            f << b;
        }
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

} // namespace altair::obi_select
