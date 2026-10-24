// Tests for strategies/vol_of_vol.hpp -- short the wings, long the money.
//
// A synthetic market where the answer is known: the forward moves at a fixed
// realised vol equal to the ATM vol, and options are priced off a fixed smile.
// Rich wings (a smile above the ATM vol) are then over-priced by
// construction, so selling vol-of-vol must earn, a flat smile must earn
// about nothing, and the mirror must lose exactly what the trade makes.

#include <strategies/vol_of_vol.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace vov = altair::vol_of_vol;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

struct Gen {
    std::uint64_t s;
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0); }
    double n() { double a = 0; for (int k = 0; k < 12; ++k) a += u(); return a - 6.0; }
};

struct World {
    std::vector<std::int64_t> day;
    std::vector<double> fwd;
    std::vector<double> curv;   ///< the smile's curvature at each close (wing richness)
};

World make(std::size_t n, double atm, double curv, std::uint64_t seed, bool alternate = false) {
    World w;
    Gen g{seed};
    std::int64_t d = 20'000;
    double f = 25'000.0;
    for (std::size_t i = 0; i < n; ++i) {
        while (((d + 3) % 7 + 7) % 7 >= 5) ++d;   // weekdays
        w.day.push_back(d);
        w.fwd.push_back(f);
        w.curv.push_back(alternate ? ((i / 20) % 2 == 0 ? curv : 0.0) : curv);
        f *= std::exp(atm / std::sqrt(252.0) * g.n() - 0.5 * atm * atm / 252.0);
        ++d;
    }
    return w;
}

vov::VovMarket market(const World& w, double atm) {
    vov::VovMarket m;
    m.day = w.day;
    for (std::size_t i = 25; i < w.day.size() + 40; i += 21) m.expiries.insert(w.day.front() + static_cast<std::int64_t>(i * 7 / 5));
    const World* pw = &w;
    m.forward = [pw](std::size_t i, std::int64_t) { return pw->fwd[i]; };
    m.chain = [pw, atm](std::size_t i, std::int64_t e) {
        std::vector<vov::VovStrike> out;
        const double F = pw->fwd[i];
        const double T = static_cast<double>(e - pw->day[i]) / 365.0;
        if (!(T > 0)) return out;
        for (double k = std::round(F * 0.85 / 100) * 100; k <= F * 1.15; k += 100) {
            // Wings dearer the farther out, in standard deviations: curv 0.03
            // puts the 25-delta butterfly near 1.35 vol points, NIFTY-like.
            const double z = std::log(k / F) / (atm * std::sqrt(T));
            const double vol = atm + pw->curv[i] * z * z;
            vov::VovStrike s;
            s.k = k;
            s.call = altair::detail::black76_unchecked(altair::OptionRight::Call, F, k, T, vol, 0.0).price;
            s.put = altair::detail::black76_unchecked(altair::OptionRight::Put, F, k, T, vol, 0.0).price;
            out.push_back(s);
        }
        return out;
    };
    return m;
}

struct Totals { double gross = 0; std::size_t n = 0, wins = 0; double worst_vega = 0; };
Totals total(const vov::VovResult& r) {
    Totals t;
    for (const auto& x : r.trades) { t.gross += x.gross; ++t.n; if (x.gross > 0) ++t.wins; t.worst_vega = std::max(t.worst_vega, std::fabs(x.vega_left)); }
    return t;
}

} // namespace

int main() {
    std::printf("Vol of vol: short the wings, long the money\n");
    const double atm = 0.13;
    vov::VovPolicy p;
    p.lot_size = 75;
    p.rate = 0.0;
    p.slippage_pts = 0.0;

    const World rich = make(500, atm, 0.03, 7);
    const World flat = make(500, atm, 0.0, 7);
    const auto mr = market(rich, atm), mf = market(flat, atm);
    const auto rr = vov::run(mr, p);
    const auto rf = vov::run(mf, p);
    check(rr && rf, "the engine runs on both markets");
    if (!rr || !rf) return 1;
    const Totals tr = total(*rr), tf = total(*rf);
    std::printf("        rich wings: %zu trades, %zu won, gross Rs %.0f; flat smile: %zu trades, gross Rs %.0f\n", tr.n, tr.wins,
                tr.gross, tf.n, tf.gross);
    check(tr.n >= 15 && tr.gross > 0 && tr.wins * 2 > tr.n,
          "wings priced above the vol that realises: selling vol-of-vol earns, in more than half the weeks");
    check(std::fabs(tf.gross) < 0.25 * tr.gross, "a flat smile at the realised vol earns about nothing (under a quarter of that)");

    vov::VovPolicy mirror = p;
    mirror.side = 1;
    const auto rm = vov::run(mr, mirror);
    check(rm && std::fabs(total(*rm).gross + tr.gross) < 1e-6 * std::max(1.0, std::fabs(tr.gross)),
          "the mirror (long vol-of-vol) loses exactly what the trade makes");

    // Vega-neutral to within half a straddle lot's vega.
    {
        const auto g = vov::detail::greeks(true, 25'000, 25'000, 20.0 / 365.0, atm, 0.0);
        check(tr.worst_vega <= 0.5 * 2 * g.vega * p.lot_size + 1e-9, "sized vega-neutral: what is left is under half a straddle lot's vega");
        bool short_volga = true;
        for (const auto& x : rr->trades) short_volga = short_volga && x.volga < 0 && x.strangle_units < 0 && x.straddle_units > 0;
        check(short_volga, "every trade is short the strangle, long the straddle, and short volga");
    }

    // The gate: with rich wings half the time, it trades the rich half.
    {
        const World alt = make(600, atm, 0.03, 11, true);
        const auto ma = market(alt, atm);
        vov::VovPolicy gp = p;
        gp.gate = vov::VovGate::RichWings;
        const auto ra = vov::run(ma, gp);
        const auto rall = vov::run(ma, p);
        std::size_t in_rich = 0;
        if (ra) for (const auto& x : ra->trades) if (alt.curv[x.entry] > 0) ++in_rich;
        std::printf("        gate: %zu trades (%zu in rich weeks), always: %zu; gross gated Rs %.0f vs always Rs %.0f\n",
                    ra ? ra->trades.size() : 0, in_rich, rall ? rall->trades.size() : 0, ra ? total(*ra).gross : 0.0,
                    rall ? total(*rall).gross : 0.0);
        const double per_gated = ra && !ra->trades.empty() ? total(*ra).gross / static_cast<double>(ra->trades.size()) : 0.0;
        const double per_all = rall && !rall->trades.empty() ? total(*rall).gross / static_cast<double>(rall->trades.size()) : 0.0;
        check(ra && rall && ra->trades.size() < rall->trades.size() && in_rich * 4 >= ra->trades.size() * 3 && ra->gated > 0
                  && per_gated > per_all,
              "the rich-wings gate trades mostly when the wings are dear (half the weeks are), and earns more a trade");
    }

    // No look-ahead: a different future after day c changes nothing decided before it.
    {
        World cut = rich;
        const std::size_t c = 300;
        for (std::size_t i = c + 1; i < cut.fwd.size(); ++i) { cut.fwd[i] *= 1.07; cut.curv[i] = 0.0; }
        const auto rc = vov::run(market(cut, atm), p);
        bool same = rc.has_value();
        std::size_t compared = 0;
        if (rc)
            for (std::size_t k = 0; k < rr->trades.size() && k < rc->trades.size(); ++k) {
                const auto& a = rr->trades[k];
                const auto& b = rc->trades[k];
                if (a.entry > c) break;
                same = same && a.entry == b.entry && a.k_put == b.k_put && a.k_call == b.k_call && a.straddle_units == b.straddle_units;
                if (a.exit <= c) same = same && a.gross == b.gross;
                ++compared;
            }
        check(same && compared > 5, "every entry up to a close, and every trade closed by then, is the same whatever comes after");
    }

    std::printf("Vol of vol: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
