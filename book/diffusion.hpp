// book/diffusion.hpp -- information diffusion between two contracts: who
// moves first, by how long, and how much of the move the follower still owes.
//
// THE IDEA (Hong and Stein 1999; Hou and Moskowitz 2005; Kyle 1985). News
// does not reach every contract at once. On NSE it usually lands on the index
// future first and reaches the index, the stocks and the option quotes a few
// hundred milliseconds to seconds later. While it travels, the follower's
// quote is stale: that gap is the trade.
//
// THREE MEASUREMENTS, one PairStats per leader/follower pair, fed every
// price change of either side on the feed's clock (no wall clock, so a tape
// replay computes exactly what the live session did):
//
//   1. Lead-lag profile. The Hayashi-Yoshida (2005) covariance of the two
//      log-price change series with the leader shifted by each lag of a grid
//      (default +/- 0..5 s). It works on the raw, unevenly spaced changes:
//      nothing is resampled, so no lag is invented by a sampling grid. The
//      lag of the largest |correlation| is the diffusion delay; the
//      lead-lag ratio LLR = sum rho^2 (leader first) / sum rho^2 (follower
//      first) of Huth and Abergel (2014) says which side leads (> 1: the
//      leader does). beta = cov(best lag) / var(leader): how far the follower
//      moves, in log terms, per unit of the leader's move (about 1 for an
//      index against its future, the option's elasticity for an option).
//   2. Price discovery share (only for pairs that are the same asset: future
//      against index or stock). A two-variable error-correction fit on
//      log prices sampled every second, z = log L - log F (demeaned):
//          dL = aL z + eL,   dF = aF z + eF.
//      Gonzalo-Granger component share of the leader = aF / (aF - aL);
//      Hasbrouck (1995) information share bounds from the two Cholesky
//      orderings of the residual covariance.
//   3. The gap. With H = max(best lag, min_horizon):
//          gap = beta * (log L_t - log L_{t-2H}) - (log F_t - log F_{t-H})
//      what the leader did in its last H is still owed, plus whatever the
//      follower's last H fell short of answering the H before it.
//      the follower's fair price = F_t * exp(gap). Its half-life comes from an
//      AR(1) fit of the gap sampled every second (an Ornstein-Uhlenbeck
//      process read discretely): the diffusion speed.
//
// NOT VALIDATED. Nothing here has been fitted out of sample; the live gate
// (live/diffusion.hpp) and the daily report are the test. FYERS adds
// milliseconds and every socket its own jitter: a lag below the measured
// jitter is noise, which is why the gate has a floor.
//
// THREADING: plain data, no locks; the live engine calls it on its one feed
// thread.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <vector>

namespace altair::diffusion {

inline constexpr std::int64_t kMsNs = 1'000'000LL;
inline constexpr std::int64_t kSecNs = 1'000'000'000LL;

struct Config {
    /// Lags tried with the leader first; the mirror (follower first) is added.
    std::vector<std::int64_t> lags_ns{0,           50 * kMsNs,   100 * kMsNs,  200 * kMsNs,
                                      300 * kMsNs, 500 * kMsNs,  750 * kMsNs,  1000 * kMsNs,
                                      1500 * kMsNs, 2000 * kMsNs, 3000 * kMsNs, 5000 * kMsNs};
    std::int64_t window_ns = 300 * kSecNs;      ///< the lead-lag window
    std::int64_t recompute_ns = kSecNs;         ///< the profile is refreshed at most this often
    std::int64_t sample_ns = kSecNs;            ///< the grid of the gap and the discovery fit
    std::size_t max_samples = 900;              ///< 15 minutes of seconds
    std::size_t min_moves = 20;                 ///< price changes each side before anything is read
    std::size_t min_samples = 60;               ///< seconds before the half-life or shares are read
    std::int64_t min_horizon_ns = 250 * kMsNs;  ///< the gap's shortest horizon
    bool same_asset = false;                    ///< compute the discovery shares (future vs index or stock)
};

struct LeadLag {
    bool valid = false;
    std::size_t leader_moves = 0, follower_moves = 0;
    std::vector<std::int64_t> lags_ns;  ///< the full grid, follower-first lags negative, ascending
    std::vector<double> rho;            ///< correlation at each lag
    std::int64_t best_lag_ns = 0;       ///< > 0: the leader moves first
    double best_rho = 0.0;
    double llr = 1.0;                   ///< lead-lag ratio, > 1: the leader leads
    double beta = 0.0;                  ///< follower log move per unit leader log move, at the best lag
};

struct Discovery {
    bool valid = false;
    double cs_leader = std::numeric_limits<double>::quiet_NaN();     ///< Gonzalo-Granger component share
    double is_leader_lo = std::numeric_limits<double>::quiet_NaN();  ///< Hasbrouck information share, lower bound
    double is_leader_hi = std::numeric_limits<double>::quiet_NaN();  ///< and upper bound
    double alpha_leader = 0.0, alpha_follower = 0.0;
};

struct Gap {
    bool valid = false;
    double log_gap = 0.0;       ///< what the follower still owes, in log terms
    double fair = 0.0;          ///< the follower's fair price, rupees
    double rupees = 0.0;        ///< fair - follower price, rupees a unit
    std::int64_t horizon_ns = 0;
};

/// One leader/follower pair: feed it prices, read the three measurements.
class PairStats {
public:
    explicit PairStats(Config c = {}) : c_(std::move(c)) {
        std::vector<std::int64_t> g;
        for (const auto l : c_.lags_ns) { g.push_back(l); if (l != 0) g.push_back(-l); }
        std::sort(g.begin(), g.end());
        g.erase(std::unique(g.begin(), g.end()), g.end());
        ll_.lags_ns = std::move(g);
        ll_.rho.assign(ll_.lags_ns.size(), 0.0);
        max_lag_ = ll_.lags_ns.empty() ? 0 : std::max(std::llabs(ll_.lags_ns.front()), std::llabs(ll_.lags_ns.back()));
    }

    [[nodiscard]] const Config& config() const noexcept { return c_; }

    void on_leader(double price, std::int64_t ns) { update(l_, price, ns); }
    void on_follower(double price, std::int64_t ns) { update(f_, price, ns); }

    [[nodiscard]] const LeadLag& lead_lag() const noexcept { return ll_; }
    [[nodiscard]] const Discovery& discovery() const noexcept { return disc_; }
    /// Seconds for half the gap to close; infinity while unknown or not closing.
    [[nodiscard]] double half_life_s() const noexcept { return half_life_s_; }
    [[nodiscard]] double leader_price() const noexcept { return l_.has ? std::exp(l_.logp) : 0.0; }
    [[nodiscard]] double follower_price() const noexcept { return f_.has ? std::exp(f_.logp) : 0.0; }
    [[nodiscard]] std::size_t samples() const noexcept { return samples_.size(); }

    /// The gap now (at the last update's time).
    [[nodiscard]] Gap gap() const { return gap_at(now_); }

private:
    struct Point { std::int64_t t; double logp; };
    struct Move { std::int64_t t0, t1; double r; };
    struct Side {
        bool has = false;
        double logp = 0.0;
        std::int64_t t = 0;
        std::deque<Move> moves;   ///< (t0, t1]: the change from the last different price
        std::deque<Point> path;   ///< every change, for the gap's look-back
        double sum_sq = 0.0;
    };
    struct Sample { double l, f, gap; };

    void update(Side& s, double price, std::int64_t ns) {
        if (!(price > 0.0) || !std::isfinite(price)) return;
        if (ns < now_) ns = now_;   // the feed clock never runs back here
        now_ = ns;
        const double lp = std::log(price);
        if (!s.has) {
            s.has = true; s.logp = lp; s.t = ns;
            s.path.push_back({ns, lp});
        } else if (lp != s.logp) {
            const double r = lp - s.logp;
            s.moves.push_back({s.t, ns, r});
            s.sum_sq += r * r;
            s.logp = lp; s.t = ns;
            s.path.push_back({ns, lp});
        }
        trim(l_);
        trim(f_);
        sample();
        if (ns - last_compute_ >= c_.recompute_ns) {
            last_compute_ = ns;
            compute_lead_lag();
            compute_samples_fits();
        }
    }

    void trim(Side& s) {
        const std::int64_t cut = now_ - c_.window_ns;
        while (!s.moves.empty() && s.moves.front().t1 < cut) {
            s.sum_sq -= s.moves.front().r * s.moves.front().r;
            s.moves.pop_front();
        }
        if (s.moves.empty()) s.sum_sq = 0.0;   // no drift from repeated subtraction
        // Keep one point at or before the longest look-back.
        const std::int64_t keep = now_ - 2 * std::max<std::int64_t>(max_lag_, c_.min_horizon_ns) - c_.sample_ns;
        while (s.path.size() > 1 && s.path[1].t <= keep) s.path.pop_front();
    }

    /// The log price at or before `t`; false if the side had none yet.
    [[nodiscard]] static bool at(const Side& s, std::int64_t t, double& lp) {
        if (s.path.empty() || s.path.front().t > t) return false;
        auto it = std::upper_bound(s.path.begin(), s.path.end(), t, [](std::int64_t v, const Point& p) { return v < p.t; });
        lp = std::prev(it)->logp;
        return true;
    }

    [[nodiscard]] Gap gap_at(std::int64_t t) const {
        Gap g;
        if (!ll_.valid || !l_.has || !f_.has) return g;
        const std::int64_t h = std::max(ll_.best_lag_ns, c_.min_horizon_ns);
        double l0 = 0.0, f0 = 0.0;
        // The leader over two lags, the follower over the last one: the
        // follower's last lag answers the leader's lag before it, so what the
        // leader did in its last lag is still owed, plus any shortfall in the
        // answer to the lag before.
        if (!at(l_, t - 2 * h, l0) || !at(f_, t - h, f0)) return g;
        g.valid = true;
        g.horizon_ns = h;
        g.log_gap = ll_.beta * (l_.logp - l0) - (f_.logp - f0);
        const double fp = std::exp(f_.logp);
        g.fair = fp * std::exp(g.log_gap);
        g.rupees = g.fair - fp;
        return g;
    }

    void sample() {
        if (!l_.has || !f_.has) return;
        if (next_sample_ == 0) next_sample_ = (now_ / c_.sample_ns + 1) * c_.sample_ns;
        while (now_ >= next_sample_) {
            const Gap g = gap_at(next_sample_);
            double lp = 0.0, fp = 0.0;
            if (at(l_, next_sample_, lp) && at(f_, next_sample_, fp))
                samples_.push_back({lp, fp, g.valid ? g.log_gap : std::numeric_limits<double>::quiet_NaN()});
            while (samples_.size() > c_.max_samples) samples_.pop_front();
            next_sample_ += c_.sample_ns;
            // A gap in the feed (lunch, a disconnect): jump, do not fill seconds one by one.
            if (now_ - next_sample_ > 10 * c_.sample_ns) next_sample_ = (now_ / c_.sample_ns) * c_.sample_ns;
        }
    }

    /// Hayashi-Yoshida covariance with the leader shifted later by `lag`.
    [[nodiscard]] double hy(std::int64_t lag) const {
        double s = 0.0;
        std::size_t j0 = 0;
        const auto& fm = f_.moves;
        for (const Move& a : l_.moves) {
            const std::int64_t a0 = a.t0 + lag, a1 = a.t1 + lag;
            while (j0 < fm.size() && fm[j0].t1 <= a0) ++j0;
            for (std::size_t j = j0; j < fm.size() && fm[j].t0 < a1; ++j) s += a.r * fm[j].r;
        }
        return s;
    }

    void compute_lead_lag() {
        ll_.leader_moves = l_.moves.size();
        ll_.follower_moves = f_.moves.size();
        ll_.valid = false;
        if (ll_.leader_moves < c_.min_moves || ll_.follower_moves < c_.min_moves || !(l_.sum_sq > 0.0) || !(f_.sum_sq > 0.0)) return;
        const double norm = std::sqrt(l_.sum_sq * f_.sum_sq);
        double best = -1.0, lead = 0.0, follow = 0.0;
        std::size_t bi = 0;
        for (std::size_t k = 0; k < ll_.lags_ns.size(); ++k) {
            const double rho = hy(ll_.lags_ns[k]) / norm;
            ll_.rho[k] = rho;
            const double a = std::fabs(rho);
            // Ties go to the shorter lag: the grid is ascending, so compare on |lag|.
            if (a > best + 1e-12 || (std::fabs(a - best) <= 1e-12 && std::llabs(ll_.lags_ns[k]) < std::llabs(ll_.lags_ns[bi]))) { best = a; bi = k; }
            if (ll_.lags_ns[k] > 0) lead += rho * rho;
            else if (ll_.lags_ns[k] < 0) follow += rho * rho;
        }
        ll_.best_lag_ns = ll_.lags_ns[bi];
        ll_.best_rho = ll_.rho[bi];
        ll_.llr = follow > 0.0 ? lead / follow : (lead > 0.0 ? 1e9 : 1.0);
        ll_.beta = hy(ll_.best_lag_ns) / l_.sum_sq;
        ll_.valid = true;
    }

    void compute_samples_fits() {
        // Half-life: AR(1) of the demeaned gap.
        half_life_s_ = std::numeric_limits<double>::infinity();
        std::vector<double> g;
        g.reserve(samples_.size());
        for (const auto& s : samples_) if (std::isfinite(s.gap)) g.push_back(s.gap);
        if (g.size() >= c_.min_samples) {
            double m = 0.0;
            for (const double v : g) m += v;
            m /= static_cast<double>(g.size());
            double num = 0.0, den = 0.0;
            for (std::size_t i = 1; i < g.size(); ++i) { num += (g[i] - m) * (g[i - 1] - m); den += (g[i - 1] - m) * (g[i - 1] - m); }
            const double phi = den > 0.0 ? num / den : 1.0;
            const double step_s = static_cast<double>(c_.sample_ns) / 1e9;
            if (phi <= 0.0) half_life_s_ = step_s * 0.5;   // gone within one sample
            else if (phi < 1.0) half_life_s_ = step_s * std::log(2.0) / -std::log(phi);
        }
        if (c_.same_asset) fit_discovery();
    }

    void fit_discovery() {
        disc_ = Discovery{};
        const std::size_t n = samples_.size();
        if (n < c_.min_samples + 1) return;
        double zm = 0.0;
        for (const auto& s : samples_) zm += s.l - s.f;
        zm /= static_cast<double>(n);
        // OLS with an intercept: regress dL and dF on z_{t-1}.
        double sz = 0.0, szz = 0.0, sl = 0.0, sf = 0.0, szl = 0.0, szf = 0.0;
        const double m = static_cast<double>(n - 1);
        for (std::size_t i = 1; i < n; ++i) {
            const double z = samples_[i - 1].l - samples_[i - 1].f - zm;
            const double dl = samples_[i].l - samples_[i - 1].l, df = samples_[i].f - samples_[i - 1].f;
            sz += z; szz += z * z; sl += dl; sf += df; szl += z * dl; szf += z * df;
        }
        const double vz = szz - sz * sz / m;
        if (!(vz > 0.0)) return;
        const double al = (szl - sz * sl / m) / vz, af = (szf - sz * sf / m) / vz;
        const double cl = (sl - al * sz) / m, cf = (sf - af * sz) / m;
        double oll = 0.0, off = 0.0, olf = 0.0;
        for (std::size_t i = 1; i < n; ++i) {
            const double z = samples_[i - 1].l - samples_[i - 1].f - zm;
            const double el = samples_[i].l - samples_[i - 1].l - cl - al * z;
            const double ef = samples_[i].f - samples_[i - 1].f - cf - af * z;
            oll += el * el; off += ef * ef; olf += el * ef;
        }
        oll /= m; off /= m; olf /= m;
        const double d = af - al;
        if (std::fabs(d) < 1e-12 || !(oll > 0.0) || !(off > 0.0)) return;
        const double gl = af / d, gf = -al / d;
        const double total = gl * gl * oll + 2.0 * gl * gf * olf + gf * gf * off;
        if (!(total > 0.0)) return;
        // Leader first in the Cholesky ordering: the upper bound.
        const double f11 = std::sqrt(oll), f21 = olf / f11;
        const double hi = (gl * f11 + gf * f21) * (gl * f11 + gf * f21) / total;
        // Follower first: the lower bound.
        const double g11 = std::sqrt(off), g21 = olf / g11, g22sq = oll - g21 * g21;
        const double lo = g22sq > 0.0 ? gl * gl * g22sq / total : 0.0;
        disc_.valid = true;
        disc_.alpha_leader = al;
        disc_.alpha_follower = af;
        disc_.cs_leader = gl;
        disc_.is_leader_lo = std::min(lo, hi);
        disc_.is_leader_hi = std::max(lo, hi);
    }

    Config c_;
    Side l_, f_;
    LeadLag ll_;
    Discovery disc_;
    std::deque<Sample> samples_;
    double half_life_s_ = std::numeric_limits<double>::infinity();
    std::int64_t now_ = 0, last_compute_ = std::numeric_limits<std::int64_t>::min() / 2, next_sample_ = 0, max_lag_ = 0;
};

} // namespace altair::diffusion
