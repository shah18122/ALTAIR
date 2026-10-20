// app/iv_record.hpp -- the at-the-money implied vol of each minute, from the
// FYERS option chain the terminal streamed.
//
// WHAT IT READS. A session tape (live/tape.hpp) holds every quote and trade
// the price service put on the bus, with the session's universe: the index,
// its nearest future and the nearest expiry's calls and puts, ATM +- 20
// strikes (live/universe.hpp). That is the FYERS chain as it traded.
//
// WHAT IT WRITES. At the end of each minute, for each index: among the
// nearest expiry's strikes with both a call and a put priced in the last 60
// seconds, the one where they are closest (at the money); the forward from
// put-call parity there, F = K + (C - P) e^(rT) -- the options' own forward,
// so a weekly expiry does not borrow the monthly future's carry; and the
// Black-76 implied vol of the call and of the put, averaged. A price is the
// mid of the best bid and ask when both stand, else the last trade. A minute
// without a fresh pair, or with under a day to the expiry, is skipped: the
// last day's options are mostly gamma and noise.
//
// The readings are stamped at the END of their minute -- the moment they are
// known -- and app/ivrv_feed.hpp joins them as of each decision.

#pragma once

#include <analytics/iv.hpp>
#include <live/universe.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace altair::iv_record {

inline constexpr std::int64_t kIstOffsetSec = 19'800;
inline constexpr std::int64_t kFreshSec = 60;
inline constexpr std::int64_t kExpirySec = 15 * 3600 + 30 * 60;   ///< options expire at the 15:30 close

/// One minute's reading.
struct Minute {
    std::int64_t t = 0;          ///< IST seconds (dataset convention): the end of the minute
    double iv = 0.0;             ///< annualised
    double forward = 0.0, strike = 0.0;
    std::int64_t expiry_day = 0;
};

class AtmIvRecorder {
public:
    AtmIvRecorder(const std::vector<live::LiveInstrument>& universe, std::string underlying, double rate = 0.065)
        : underlying_(std::move(underlying)), rate_(rate) {
        for (const auto& in : universe) {
            if (in.underlying != underlying_) { continue; }
            if (in.kind == live::LiveKind::Call || in.kind == live::LiveKind::Put) {
                legs_[in.token] = {in.expiry_day, in.strike, in.kind == live::LiveKind::Call};
            }
        }
    }

    /// A price for `token` (rupees) at UTC epoch nanoseconds `ns`.
    void on_price(std::uint32_t token, double rupees, std::int64_t ns) {
        if (!(rupees > 0.0)) { return; }
        const std::int64_t ist = ns / 1'000'000'000 + kIstOffsetSec;
        const std::int64_t minute = ist / 60;
        if (minute_ >= 0 && minute != minute_) { close_minute(); }
        minute_ = minute;
        const auto leg = legs_.find(token);
        if (leg != legs_.end()) { price_[token] = {rupees, ist}; }
    }

    /// Close the minute in progress (at the end of a tape).
    void finish() {
        if (minute_ >= 0) { close_minute(); }
        minute_ = -1;
    }

    [[nodiscard]] const std::vector<Minute>& minutes() const noexcept { return out_; }

private:
    struct Leg { std::int64_t expiry = 0; double strike = 0.0; bool call = false; };
    struct Px { double v = 0.0; std::int64_t at = 0; };

    void close_minute() {
        const std::int64_t end = (minute_ + 1) * 60;
        // The nearest expiry's fresh calls and puts by strike.
        std::int64_t expiry = 0;
        for (const auto& [tok, leg] : legs_) {
            const auto p = price_.find(tok);
            if (p == price_.end() || end - p->second.at > kFreshSec) { continue; }
            if (expiry == 0 || leg.expiry < expiry) { expiry = leg.expiry; }
        }
        if (expiry == 0) { return; }
        const double T = static_cast<double>(expiry * 86'400 + kExpirySec - end) / (365.0 * 86'400.0);
        if (T < 1.0 / 365.0) { return; }
        std::map<double, std::pair<double, double>> pair;   // strike -> (call, put)
        for (const auto& [tok, leg] : legs_) {
            if (leg.expiry != expiry) { continue; }
            const auto p = price_.find(tok);
            if (p == price_.end() || end - p->second.at > kFreshSec) { continue; }
            auto& cp = pair[leg.strike];
            (leg.call ? cp.first : cp.second) = p->second.v;
        }
        double best = std::numeric_limits<double>::infinity(), K = 0.0, c = 0.0, p = 0.0;
        for (const auto& [k, cp] : pair) {
            if (!(cp.first > 0.0) || !(cp.second > 0.0)) { continue; }
            if (std::fabs(cp.first - cp.second) < best) { best = std::fabs(cp.first - cp.second); K = k; c = cp.first; p = cp.second; }
        }
        if (!(K > 0.0)) { return; }
        const double F = K + (c - p) * std::exp(rate_ * T);
        if (!(F > 0.0)) { return; }
        double sum = 0.0;
        int n = 0;
        for (const bool call : {true, false}) {
            const auto v = altair::implied_vol_black76(call ? altair::OptionRight::Call : altair::OptionRight::Put,
                                                       altair::Price{std::llround((call ? c : p) * 100.0)},
                                                       altair::Price{std::llround(F * 100.0)},
                                                       altair::Price{std::llround(K * 100.0)}, altair::Years{T}, rate_);
            if (v && v->vol.raw() > 0.0 && std::isfinite(v->vol.raw())) { sum += v->vol.raw(); ++n; }
        }
        if (n == 0) { return; }
        out_.push_back({end, sum / n, F, K, expiry});
    }

    std::string underlying_;
    double rate_;
    std::map<std::uint32_t, Leg> legs_;
    std::map<std::uint32_t, Px> price_;
    std::int64_t minute_ = -1;
    std::vector<Minute> out_;
};

} // namespace altair::iv_record
