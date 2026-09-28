// strategies/pairs_trade.hpp -- deterministic pair entry/exit and costed P&L.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

enum class PairTradeError : std::uint8_t { BadParameter, BadSeries, NonFinite };

struct PairTradePolicy {
    double entry_z = 2.0;
    double exit_z = 0.25;
    double stop_z = 4.0;
    std::size_t max_hold_bars = 50;
    double gross_notional = 100'000.0;
    double round_trip_cost_bps = 5.0;
};

struct PairRoundTrip {
    std::size_t entry_bar = 0;
    std::size_t exit_bar = 0;
    int spread_side = 0; // +1 long y-short beta*x, -1 the reverse
    double entry_z = 0.0;
    double exit_z = 0.0;
    double gross_pnl = 0.0;
    double cost = 0.0;
    double net_pnl = 0.0;
    bool stopped = false;
};

/// Replay one fixed, previously-estimated cointegrating relationship.
/// `z` must be computed causally by the caller. The two legs are sized to
/// equal cash notionals, so no hidden directional notional is introduced.
[[nodiscard]] inline std::expected<std::vector<PairRoundTrip>, PairTradeError>
replay_pair_trades(std::span<const double> y, std::span<const double> x,
                   std::span<const double> z, double hedge_beta,
                   PairTradePolicy policy) {
    if (y.size() != x.size() || y.size() != z.size() || y.size() < 2)
        return std::unexpected(PairTradeError::BadSeries);
    if (!(policy.entry_z > policy.exit_z && policy.exit_z >= 0.0)
        || !(policy.stop_z > policy.entry_z) || policy.max_hold_bars == 0
        || !(policy.gross_notional > 0.0) || policy.round_trip_cost_bps < 0.0
        || !std::isfinite(hedge_beta) || hedge_beta == 0.0)
        return std::unexpected(PairTradeError::BadParameter);
    for (std::size_t i = 0; i < y.size(); ++i)
        if (!(y[i] > 0.0) || !(x[i] > 0.0) || !std::isfinite(z[i]))
            return std::unexpected(PairTradeError::NonFinite);

    std::vector<PairRoundTrip> out;
    bool open = false;
    std::size_t entry = 0;
    int side = 0;
    double qty_y = 0.0, qty_x = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (!open && std::fabs(z[i]) >= policy.entry_z
            && std::fabs(z[i]) < policy.stop_z) {
            open = true; entry = i; side = z[i] < 0.0 ? 1 : -1;
            qty_y = 0.5 * policy.gross_notional / y[i];
            qty_x = 0.5 * policy.gross_notional / x[i];
            continue;
        }
        if (!open) continue;
        const bool mean_exit = std::fabs(z[i]) <= policy.exit_z;
        const bool stop = std::fabs(z[i]) >= policy.stop_z;
        const bool timed = i - entry >= policy.max_hold_bars;
        if (!mean_exit && !stop && !timed && i + 1 != y.size()) continue;
        PairRoundTrip trade{};
        trade.entry_bar = entry; trade.exit_bar = i; trade.spread_side = side;
        trade.entry_z = z[entry]; trade.exit_z = z[i]; trade.stopped = stop;
        // Equal-notional legs: side*y and -side*x. hedge_beta's sign chooses
        // the economic hedge direction; its magnitude was used in z upstream.
        const double hedge_direction = hedge_beta > 0.0 ? -1.0 : 1.0;
        trade.gross_pnl = static_cast<double>(side) * qty_y * (y[i] - y[entry])
                        + static_cast<double>(side) * hedge_direction * qty_x
                          * (x[i] - x[entry]);
        trade.cost = policy.gross_notional * policy.round_trip_cost_bps / 10'000.0;
        trade.net_pnl = trade.gross_pnl - trade.cost;
        out.push_back(trade);
        open = false;
    }
    return out;
}

} // namespace altair
