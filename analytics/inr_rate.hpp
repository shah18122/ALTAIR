// analytics/inr_rate.hpp -- the Indian risk-free rate every option tool defaults to.
//
// One number, in one place (NSE review, 2026-11-02: 6.5 % had been typed into
// about twenty files). It is the RBI's 91-day Treasury bill cut-off yield,
// continuously compounded closely enough at these levels: 5.5747 % at the
// auction of 7 Oct 2026. Update it when the T-bill moves by more than about
// 25 bp (RBI weekly auction results); every CLI still takes --rate.
//
// What it moves: forwards from spot (F = S e^{rT}), discounting, and the
// carry in put-call parity. For a weekly NIFTY option a 1 % rate error is
// worth well under a rupee; for monthlies a few. The live straddle reads its
// forward from parity, so the rate only scales C - P there.

#pragma once

namespace altair {

/// RBI 91-day T-bill cut-off, 2026-10-07 auction, as a fraction.
inline constexpr double kInrRiskFreeRate = 0.0557;

} // namespace altair
