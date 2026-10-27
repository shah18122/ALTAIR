// strategies/meta_book.hpp on answers known in advance: Black-Scholes values
// and put-call parity; trading time runs out exactly at the expiry's close; a
// future's P&L is the move times the lot less the charges; a flat market pays
// a straddle seller its time decay; a stop closes a losing future; a hedged
// straddle trades futures only when the delta passes half a lot; a straddle
// held to expiry settles at intrinsic value; nothing after a trade's exit
// changes it; and no strategy buys an option to open.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <strategies/meta_book.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using namespace altair::metabook;

constexpr std::int64_t kD0 = 20'000;   // an epoch day

/// A session of 375 one-minute bars following `px(i)`.
template <class F>
BookDay session(std::int64_t day, F px, double iv, int to_expiry) {
    BookDay d;
    d.day = day;
    for (int i = 0; i < 375; ++i) {
        const double p = px(i);
        d.spot.push_back({day * kBookDaySec + kSessionOpenSec + i * 60, p, p, p, p});
    }
    d.iv.assign(d.spot.size(), iv);
    d.stop_vol = iv;
    d.expiry_day = day + to_expiry;
    d.sessions_to_expiry = to_expiry;
    return d;
}

double zero_cost(bool, bool, double, double, std::int64_t) { return 0.0; }
double ten_cost(bool, bool, double, double, std::int64_t) { return 10.0; }

} // namespace

int main() {
    std::printf("altair_trader meta book\n");

    // pricing
    {
        const auto c = bs_quote(true, 100, 100, 0.25, 0.2), p = bs_quote(false, 100, 100, 0.25, 0.2);
        check(std::fabs(c.price - 3.9878) < 1e-3, "ATM call, 3 months, 20% vol: 3.988");
        check(std::fabs(c.price - p.price) < 1e-9, "put-call parity at the money with no rates");
        check(std::fabs(c.delta - p.delta - 1.0) < 1e-12, "call delta minus put delta is 1");
        check(bs_quote(true, 110, 100, 0, 0.2).price == 10.0 && bs_quote(false, 110, 100, 0, 0.2).price == 0.0,
              "at expiry, intrinsic value");
    }
    {
        BookDay d = session(kD0, [](int) { return 100.0; }, 0.2, 0);
        check(tau_years(d, kD0 * kBookDaySec + kSessionOpenSec + 375 * 60) == 0.0, "no time left at the expiry's close");
        d.sessions_to_expiry = 2;
        check(std::fabs(tau_years(d, kD0 * kBookDaySec + kSessionOpenSec) * kBookTradingDays - (2 + 1 - kOvernightShare)) < 1e-12,
              "at the open, two sessions out: 2.8 trading days");
    }

    BookConfig cfg;
    cfg.lot = 50;
    cfg.strike_step = 50;
    cfg.fut_slip_pts = 0;
    cfg.opt_slip_min = 0;
    cfg.opt_slip_pct = 0;

    // a future: up 1 point a minute
    {
        std::vector<BookDay> days{session(kD0, [](int i) { return 20000.0 + i; }, 0.15, 3)};
        MetaBook book(days, cfg, ten_cost);
        const auto r = book.run(static_cast<int>(BookStrategy::FutLong), 0, 75);   // 10:30
        const std::size_t x = MetaBook::square_off_bar(days[0]);
        check(std::fabs(r.gross - (static_cast<double>(x) - 75.0) * 50.0) < 1e-9, "long future: the move times the lot");
        check(r.pnl == r.gross - 20.0 && !r.synthetic, "less two fills' charges; FYERS-priced, not synthetic");
        const auto s = book.run(static_cast<int>(BookStrategy::FutShortStop), 0, 75);
        check(s.why.find("stop") != std::string::npos && s.gross < 0, "a short future against the move is stopped");
        check(s.known_at < r.known_at, "a stopped trade is final when stopped, before the square-off");
    }

    // a flat market: the straddle seller keeps the time decay
    {
        std::vector<BookDay> days{session(kD0, [](int) { return 20000.0; }, 0.15, 1)};
        MetaBook book(days, cfg, zero_cost);
        const auto r = book.run(static_cast<int>(BookStrategy::Straddle), 0, 15);
        check(r.synthetic && r.gross > 0, "flat market: a short straddle earns its decay");
        const auto h = book.run(static_cast<int>(BookStrategy::StraddleHedged), 0, 15);
        check(h.why.find(", 0 futures hedges") != std::string::npos && std::fabs(h.gross - r.gross) < 1e-9,
              "flat market: the hedged straddle never trades a future");
        const auto g = book.run(static_cast<int>(BookStrategy::Strangle), 0, 15);
        check(g.gross > 0 && g.gross < r.gross, "a strangle earns less decay than the straddle");
    }

    // a trend: the hedge trades, and the unhedged straddle loses more
    {
        std::vector<BookDay> days{session(kD0, [](int i) { return 20000.0 + 2.0 * i; }, 0.15, 1)};
        MetaBook book(days, cfg, zero_cost);
        const auto r = book.run(static_cast<int>(BookStrategy::Straddle), 0, 15);
        const auto h = book.run(static_cast<int>(BookStrategy::StraddleHedged), 0, 15);
        const auto s = book.run(static_cast<int>(BookStrategy::StraddleStop), 0, 15);
        check(r.gross < 0, "a 700-point trend: the short straddle loses");
        check(h.why.find(", 0 futures hedges") == std::string::npos && h.gross > r.gross,
              "the delta hedge trades futures and loses less");
        check(s.why.find("premium stop") != std::string::npos && s.gross > r.gross, "the premium stop cuts the loss");
    }

    // held to expiry: settles at intrinsic value
    {
        std::vector<BookDay> days{session(kD0, [](int) { return 20000.0; }, 0.15, 1),
                                  session(kD0 + 1, [](int i) { return i < 374 ? 20000.0 : 20300.0; }, 0.15, 0)};
        days[0].expiry_day = kD0 + 1;
        days[1].expiry_day = kD0 + 1;
        MetaBook book(days, cfg, zero_cost);
        const auto r = book.run(static_cast<int>(BookStrategy::StraddleToExpiry), 0, 15);
        check(r.why.find("settled at expiry at 300.00") != std::string::npos, "settled at the last close's intrinsic value");
        check(r.known_at == days[1].spot.back().t + 60, "final at the expiry session's close");
    }

    // nothing after the exit moves a trade
    {
        std::vector<BookDay> a{session(kD0, [](int i) { return 20000.0 + 3.0 * std::sin(i / 7.0); }, 0.15, 2)};
        std::vector<BookDay> b = a;
        const std::size_t x = MetaBook::square_off_bar(a[0]);
        for (std::size_t i = x + 1; i < b[0].spot.size(); ++i) { b[0].spot[i].o *= 1.05; b[0].spot[i].c *= 1.05; b[0].iv[i] = 0.5; }
        MetaBook ba(a, cfg, ten_cost), bb(b, cfg, ten_cost);
        bool same = true;
        for (int s = 0; s < kBookStrategies; ++s) {
            if (static_cast<BookStrategy>(s) == BookStrategy::StraddleToExpiry) continue;   // held past the session
            const auto ra = ba.run(s, 0, 30), rb = bb.run(s, 0, 30);
            same = same && ra.pnl == rb.pnl && ra.known_at == rb.known_at;
        }
        check(same, "bars after the 15:20 square-off move no intraday strategy");
    }

    // sell-only: every option fill that opens is a sale
    {
        std::vector<BookDay> days{session(kD0, [](int i) { return 20000.0 + 2.0 * i; }, 0.15, 1),
                                  session(kD0 + 1, [](int) { return 20100.0; }, 0.15, 0)};
        days[0].expiry_day = kD0 + 1;
        days[1].expiry_day = kD0 + 1;
        bool sell_first = true;
        for (int s = 0; s < kBookStrategies; ++s) {
            std::vector<bool> opt_buys;
            const BookCost spy = [&](bool option, bool buy, double, double, std::int64_t) {
                if (option) opt_buys.push_back(buy);
                return 0.0;
            };
            MetaBook book(days, cfg, spy);
            (void)book.run(s, 0, 30);
            // option fills: all sales come first, then only buy-backs
            bool seen_buy = false;
            for (bool buy : opt_buys) {
                if (buy) seen_buy = true;
                else if (seen_buy) sell_first = false;
            }
            if (!opt_buys.empty() && opt_buys.front()) sell_first = false;
        }
        check(sell_first, "no strategy opens by buying an option: every option is sold first, bought back to close");
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "SOME CHECKS ARE BAD");
    return failures == 0 ? 0 : 1;
}
