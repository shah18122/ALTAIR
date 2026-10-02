// live/sim.hpp -- a simulated market for the live terminal, flagged as such.
//
// WHY THIS EXISTS. The live path (FYERS socket -> price service -> terminal,
// live models, paper book) can only be exercised against FYERS between 09:15
// and 15:30 on a trading day, from a machine FYERS lets connect. Everything
// downstream of the price service is the same code whatever the source, so a
// simulated source lets all of it be built, tested and shown at any hour.
//
// IT IS NEVER MARKET DATA, AND EVERY FRAME SAYS SO: kPriceSimulated and
// kQuoteSimulated are set on everything this produces, and the terminal shows
// SIM where it would show LIVE. Nothing learned from a simulated session is
// evidence about NIFTY.
//
// THE MODEL, kept simple and stated:
//   * NIFTY: geometric Brownian motion at INDIA VIX (annualised, 252 x 22,500
//     seconds a year). BANKNIFTY: 1.25 x NIFTY's vol, correlation 0.8.
//     INDIA VIX: mean-reverting, moving against NIFTY (correlation -0.6).
//   * Futures: spot x exp(6.5 % x T) plus a little basis noise.
//   * Options: Black-76 on the simulated future, at VIX (1.2 x VIX for
//     BANKNIFTY) with a symmetric smile; quoted around that value.
//   * Stocks: beta 0.6-1.4 to NIFTY plus 20 % idiosyncratic vol. Stock
//     futures and options price off their own stock, not NIFTY.
//   * A past day (set_anchors): NIFTY, BANKNIFTY and INDIA VIX follow that
//     day's real 1-minute closes, with a Brownian bridge between them, so the
//     simulated session lands on every real close at its minute.
//   * Liquidity: indices print every second; futures and stocks on most
//     steps; options less often the further they are from the money.
// Deterministic for a given seed, so a test can pin its output.

#pragma once

#include <live/universe.hpp>

#include <analytics/greeks.hpp>
#include <server/price_payload.hpp>
#include <server/quote_payload.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace altair::live {

/// Where the simulation starts: yesterday's closes.
struct LiveSimSeeds {
    double nifty = 24000.0, banknifty = 54000.0, vix = 13.0;
    std::unordered_map<std::string, double> stocks;   ///< symbol -> last close; 1000 if absent
};

/// One instrument's update from one step: always a quote; a trade when it
/// printed; a book when it carries one.
struct LiveSimEvent {
    bool trade = false, book = false;
    PricePayload price{};
    QuotePayload quote{};
    PriceLevel bids[kMaxDepthLevels]{};
    PriceLevel asks[kMaxDepthLevels]{};
};

inline constexpr double kLiveSimRate = 0.065;
inline constexpr double kLiveSimSecondsPerYear = 252.0 * 22500.0;

class LiveSim {
public:
    LiveSim(std::vector<LiveInstrument> universe, const LiveSimSeeds& seeds, std::uint64_t seed,
            std::int64_t start_ns)
        : u_(std::move(universe)), rng_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed), now_ns_(start_ns),
          nifty_(seeds.nifty), bnf_(seeds.banknifty), vix_(seeds.vix), vix0_(seeds.vix) {
        st_.resize(u_.size());
        // Equities first: a stock's futures and options price off it.
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t i = 0; i < u_.size(); ++i) {
                const bool equity = u_[i].kind == LiveKind::Equity;
                if (equity != (pass == 0)) continue;
                const auto it = seeds.stocks.find(u_[i].symbol);
                init(i, it != seeds.stocks.end() ? it->second : 0.0);
            }
    }

    /// Add an instrument mid-session: the market watch's scrip search. Its
    /// previous close is its fair value now (`equity_close` for a stock, when
    /// known). A token already simulated is left alone.
    void add(LiveInstrument in, double equity_close = 0.0) {
        for (const auto& have : u_) if (have.token == in.token) return;
        u_.push_back(std::move(in));
        st_.emplace_back();
        init(u_.size() - 1, equity_close);
    }

    /// Follow a real day: each index lands on every one of these minute
    /// closes at its minute, on a Brownian bridge between them. The level at
    /// the start is the last close at or before it.
    void set_anchors(std::vector<LiveMinute> nifty, std::vector<LiveMinute> banknifty, std::vector<LiveMinute> vix) {
        an_[0] = std::move(nifty);
        an_[1] = std::move(banknifty);
        an_[2] = std::move(vix);
        double* level[3] = {&nifty_, &bnf_, &vix_};
        for (int k = 0; k < 3; ++k)
            for (const auto& m : an_[k])
                if (m.end_ns <= now_ns_) *level[k] = m.close;
        for (std::size_t i = 0; i < u_.size(); ++i) if (u_[i].kind != LiveKind::Equity) st_[i].fair = theo(i);
    }
    [[nodiscard]] bool anchored() const noexcept { return !an_[0].empty(); }

    [[nodiscard]] std::int64_t now_ns() const noexcept { return now_ns_; }
    [[nodiscard]] const std::vector<LiveInstrument>& universe() const noexcept { return u_; }
    [[nodiscard]] double nifty() const noexcept { return nifty_; }

    /// Advance `dt_ns` and report every instrument that changed.
    template <class Emit>
    void step(std::int64_t dt_ns, Emit&& emit) {
        now_ns_ += dt_ns;
        const double dt = static_cast<double>(dt_ns) * 1e-9 / kLiveSimSecondsPerYear;
        const double sq = std::sqrt(dt);
        const double zn = normal(), zb = 0.8 * zn + 0.6 * normal(), zv = -0.6 * zn + 0.8 * normal();
        const double sn = vix_ / 100.0;
        const double sb = 1.25 * sn;
        if (!bridge(0, nifty_, sn, zn, dt_ns)) nifty_ *= std::exp(-0.5 * sn * sn * dt + sn * sq * zn);
        if (!bridge(1, bnf_, sb, zb, dt_ns)) bnf_ *= std::exp(-0.5 * sb * sb * dt + sb * sq * zb);
        if (!bridge(2, vix_, 0.9, zv, dt_ns)) vix_ += 4.0 * (vix0_ - vix_) * dt + 0.9 * vix_ * sq * zv;
        if (!(vix_ > 5.0)) vix_ = 5.0;   // RULE 11: safe-side floor -- a simulated VIX at or below 5 has no market meaning; it only keeps the vol positive.
        second_acc_ += dt_ns;
        const bool index_print = second_acc_ >= 1'000'000'000;
        if (index_print) second_acc_ = 0;

        for (std::size_t i = 0; i < u_.size(); ++i) {
            const auto& in = u_[i];
            auto& s = st_[i];
            if (in.kind == LiveKind::Equity) {
                const double idio = 0.20 * sq * normal();
                s.fair *= std::exp(s.beta * sn * sq * zn + idio - 0.5 * (s.beta * s.beta * sn * sn + 0.04) * dt);
            } else {
                s.fair = theo(i);
            }
            bool prints = false;
            switch (in.kind) {
            case LiveKind::Index: prints = index_print; break;
            case LiveKind::Future: prints = uniform() < 0.8; break;
            case LiveKind::Equity: prints = uniform() < 0.5; break;
            default: {
                const double away = std::fabs(std::log(in.strike / underlying_future(in))) / 0.01;
                prints = uniform() < 0.6 / (1.0 + away);
                break;
            }
            }
            const bool requote = prints || (in.kind != LiveKind::Index && uniform() < 0.3);
            if (!requote) continue;
            emit(in, make_event(i, prints));
        }
    }

    /// Every instrument's current quote (and book), without advancing: the
    /// opening board, so every row has its previous close before it trades.
    template <class Emit>
    void board(Emit&& emit) {
        for (std::size_t i = 0; i < u_.size(); ++i) emit(u_[i], make_event(i, false));
    }
    template <class Emit>
    void board_one(std::size_t i, Emit&& emit) {
        if (i < u_.size()) emit(u_[i], make_event(i, false));
    }

private:
    struct State {
        double fair = 0.0, beta = 1.0;
        std::int64_t prev_close = 0, ltp = 0, open = 0, high = 0, low = 0;
        std::int64_t volume = 0, oi = 0, last_trade_ns = 0;
        double notional = 0.0;   // for the average price
        std::int64_t total_buy = 0, total_sell = 0;
    };

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t z = (rng_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    [[nodiscard]] double uniform() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
    [[nodiscard]] double normal() noexcept {
        double u1 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;   // RULE 11: proven -- only u1 == 0 is raised, and log(0) is the one input Box-Muller cannot take.
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * uniform());
    }

    [[nodiscard]] double spot_of(const LiveInstrument& in) const noexcept {
        if (in.underlying == "BANKNIFTY") return bnf_;
        if (in.underlying == "INDIAVIX") return vix_;
        if (in.underlying == "NIFTY") return nifty_;
        // A stock's derivative: the stock itself, when it is simulated.
        for (std::size_t j = 0; j < u_.size() && j < st_.size(); ++j)
            if (u_[j].kind == LiveKind::Equity && u_[j].symbol == in.underlying && st_[j].fair > 0.0) return st_[j].fair;
        return nifty_;
    }

    void init(std::size_t i, double equity_close) {
        const auto& in = u_[i];
        double px = 0.0;
        if (in.kind == LiveKind::Equity) {
            px = equity_close > 0.0 ? equity_close : 1000.0;
            st_[i].beta = 0.6 + 0.8 * uniform();
        } else {
            px = theo(i);
        }
        st_[i].fair = px;
        st_[i].prev_close = round_tick(px, in);
        st_[i].ltp = st_[i].prev_close;
        st_[i].oi = in.kind == LiveKind::Future || in.kind == LiveKind::Call || in.kind == LiveKind::Put
                        ? in.lot * static_cast<std::int64_t>(2000 + 8000 * uniform())
                        : 0;
    }

    /// One step of the Brownian bridge toward the next real close of index
    /// `k`. False when there is no close ahead (no anchors, or past the last).
    bool bridge(int k, double& level, double sigma, double z, std::int64_t dt_ns) {
        auto& a = an_[k];
        std::size_t& c = cursor_[k];
        while (c < a.size() && a[c].end_ns <= now_ns_ - dt_ns) ++c;
        if (c >= a.size() || !(level > 0.0)) return false;
        const double rem = static_cast<double>(a[c].end_ns - now_ns_);   // after this step
        const double x = std::log(level), target = std::log(a[c].close);
        if (rem <= 0.0) { level = a[c].close; return true; }
        const double step = static_cast<double>(dt_ns);
        const double var_years = step * 1e-9 / kLiveSimSecondsPerYear * rem / (rem + step);
        level = std::exp(x + (target - x) * step / (rem + step) + sigma * std::sqrt(var_years) * z);
        return true;
    }
    [[nodiscard]] double years_to(std::int64_t expiry_day) const noexcept {
        const std::int64_t close_ns = (expiry_day * 86400 + 10 * 3600) * 1'000'000'000LL;   // 15:30 IST
        const double t = static_cast<double>(close_ns - now_ns_) * 1e-9 / (365.0 * 86400.0);
        // RULE 11: safe-side floor -- at or past 15:30 on expiry the option is
        // worth its intrinsic value; one minute of time keeps Black-76 defined.
        return t < 1.0 / (365.0 * 1440.0) ? 1.0 / (365.0 * 1440.0) : t;
    }
    [[nodiscard]] double underlying_future(const LiveInstrument& in) const noexcept {
        return spot_of(in) * std::exp(kLiveSimRate * years_to(in.expiry_day));
    }
    [[nodiscard]] double theo(std::size_t i) const noexcept {
        const auto& in = u_[i];
        switch (in.kind) {
        case LiveKind::Index: return spot_of(in);
        case LiveKind::Future: return underlying_future(in);
        case LiveKind::Equity: return st_.empty() ? 1000.0 : st_[i].fair;
        default: break;
        }
        const double F = underlying_future(in);
        const double T = years_to(in.expiry_day);
        const double base = (in.underlying == "BANKNIFTY" ? 1.2 : 1.0) * vix_ / 100.0;
        const double x = std::log(in.strike / F) / (base * std::sqrt(T > 7.0 / 365.0 ? T : 7.0 / 365.0));
        const double iv = base * (1.0 + 0.04 * x * x);
        const auto g = altair::detail::black76_unchecked(
            in.kind == LiveKind::Call ? OptionRight::Call : OptionRight::Put, F, in.strike, T, iv, kLiveSimRate);
        return g.price;
    }
    [[nodiscard]] static std::int64_t round_tick(double rupees, const LiveInstrument& in) noexcept {
        const auto tick = static_cast<std::int64_t>(std::llround((in.tick > 0.0 ? in.tick : 0.05) * 100.0));
        std::int64_t p = std::llround(rupees * 100.0 / static_cast<double>(tick)) * tick;
        return p < tick ? tick : p;   // RULE 11: safe-side floor -- a listed price is never below one tick.
    }

    [[nodiscard]] LiveSimEvent make_event(std::size_t i, bool prints) {
        const auto& in = u_[i];
        auto& s = st_[i];
        LiveSimEvent ev;
        ev.price.exchange_ts_ns = now_ns_;
        const std::int64_t fair = round_tick(s.fair, in);
        const auto tick = static_cast<std::int64_t>(std::llround((in.tick > 0.0 ? in.tick : 0.05) * 100.0));
        // Half the spread: one or two ticks for futures and stocks (NIFTY
        // futures quote 0.10-0.20 wide); for options a tenth of a percent of
        // the price, and never under a tick.
        std::int64_t half = 0;
        if (in.kind == LiveKind::Future || in.kind == LiveKind::Equity) {
            half = tick * (1 + static_cast<std::int64_t>(2.0 * uniform()));
        } else if (in.kind != LiveKind::Index) {
            half = std::max<std::int64_t>(tick, (fair / 1000 / tick) * tick);   // RULE 11: safe-side floor -- a spread is at least one tick.
        }
        const std::int64_t bid = fair - half > tick ? fair - half : tick;   // RULE 11: safe-side floor -- a bid is at least one tick.
        const std::int64_t ask = bid + (half > 0 ? 2 * half : tick);
        const std::int64_t lot = in.lot > 0 ? in.lot : 1;

        if (prints) {
            const bool buy = uniform() < 0.5;
            s.ltp = in.kind == LiveKind::Index ? fair : (buy ? ask : bid);
            const std::int64_t qty = in.kind == LiveKind::Index ? 0
                                   : lot * (1 + static_cast<std::int64_t>(9.0 * uniform()));
            s.volume += qty;
            s.notional += static_cast<double>(qty) * static_cast<double>(s.ltp);
            if (s.open == 0) { s.open = s.ltp; s.high = s.ltp; s.low = s.ltp; }
            if (s.ltp > s.high) s.high = s.ltp;
            if (s.ltp < s.low) s.low = s.ltp;
            s.last_trade_ns = now_ns_;
            if (s.oi > 0) {
                s.oi += lot * static_cast<std::int64_t>(std::llround(2.0 * normal()));
                if (s.oi < lot) s.oi = lot;   // RULE 11: safe-side floor -- simulated open interest stays positive.
            }
            ev.trade = true;
            ev.price.token = in.token;
            ev.price.flags = kPriceSimulated;
            ev.price.last_paise = s.ltp;
            ev.price.last_qty = qty;
            ev.price.exchange_ts_ns = now_ns_;
            if (in.kind != LiveKind::Index) { ev.price.flags |= kPriceHasVolume; ev.price.volume = s.volume; }
            if (s.oi > 0) { ev.price.flags |= kPriceHasOi; ev.price.oi = s.oi; }
        }
        auto& q = ev.quote;
        q.token = in.token;
        q.flags = kQuoteSimulated | kQuoteHasPrevClose;
        q.prev_close = s.prev_close;
        if (s.open > 0) { q.flags |= kQuoteHasOhlc; q.open = s.open; q.high = s.high; q.low = s.low; }
        if (s.last_trade_ns > 0) { q.flags |= kQuoteHasLtt; q.last_trade_ns = s.last_trade_ns; }
        if (in.kind != LiveKind::Index) {
            q.flags |= kQuoteHasTop | kQuoteHasTotals | kQuoteHasCircuit;
            q.bid = bid; q.ask = ask;
            q.bid_qty = lot * (1 + static_cast<std::int64_t>(20.0 * uniform()));
            q.ask_qty = lot * (1 + static_cast<std::int64_t>(20.0 * uniform()));
            s.total_buy = q.bid_qty * 40; s.total_sell = q.ask_qty * 40;
            q.total_buy = s.total_buy; q.total_sell = s.total_sell;
            q.upper_circuit = s.prev_close + s.prev_close / 10;
            q.lower_circuit = s.prev_close - s.prev_close / 10;
            if (s.volume > 0) {
                q.flags |= kQuoteHasAtp;
                q.avg_price = std::llround(s.notional / static_cast<double>(s.volume));
            }
        }
        if (in.depth) {
            ev.book = true;
            ev.price.token = in.token;
            ev.price.flags |= kPriceSimulated;
            for (std::size_t k = 0; k < kMaxDepthLevels; ++k) {
                const auto step = static_cast<std::int64_t>(k) * tick;
                ev.bids[k].price_paise = bid - step > tick ? bid - step : tick;   // RULE 11: safe-side floor -- a level is at least one tick.
                ev.asks[k].price_paise = ask + step;
                ev.bids[k].qty = k == 0 ? q.bid_qty : lot * (1 + static_cast<std::int64_t>(30.0 * uniform()));
                ev.asks[k].qty = k == 0 ? q.ask_qty : lot * (1 + static_cast<std::int64_t>(30.0 * uniform()));
                ev.bids[k].orders = 1 + static_cast<std::uint32_t>(10.0 * uniform());
                ev.asks[k].orders = 1 + static_cast<std::uint32_t>(10.0 * uniform());
            }
        }
        return ev;
    }

    std::vector<LiveInstrument> u_;
    std::vector<State> st_;
    std::uint64_t rng_;
    std::int64_t now_ns_;
    std::int64_t second_acc_ = 0;
    double nifty_, bnf_, vix_, vix0_;
    std::vector<LiveMinute> an_[3];
    std::size_t cursor_[3] = {0, 0, 0};
};

/// A trade frame and a book frame share the PricePayload type but not its
/// flags: copy the trade's identity into a book-only payload.
[[nodiscard]] inline PricePayload live_book_payload(const LiveSimEvent& ev) noexcept {
    PricePayload p;
    p.token = ev.quote.token;
    p.flags = static_cast<std::uint16_t>(kPriceHasBook | kPriceSimulated);
    p.depth_levels = static_cast<std::uint16_t>(kMaxDepthLevels);
    p.exchange_ts_ns = ev.price.exchange_ts_ns;
    return p;
}

} // namespace altair::live
