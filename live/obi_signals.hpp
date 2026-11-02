// live/obi_signals.hpp -- the four order-book trade types, read off one
// book/flow.hpp FlowSignals: what the OBI trader (live/obi_trader.hpp) enters
// on and what the Order Book Imbalance page (desktop/obi_page.hpp) shows, from
// this one function, so the two can never disagree.
//
//   A  bid-side pressure -> LONG: OBI over 5 levels >= +obi_entry, held for
//      >= min_persist book updates and >= min_persist_ns, and more buying than
//      selling printed in the window;
//   B  ask-side pressure -> SHORT: the mirror;
//   C  absorption -> the REVERSAL: heavy selling into bids that refill while
//      the bid holds -> long (the mirror -> short);
//   D  liquidity withdrawal -> the BREAKOUT: asks pulled while real buys lift
//      successive asks -> long (the mirror -> short).
// C and D are the engine's own flags (book/flow.hpp, absorb_* and withdraw_*).

#pragma once

#include <book/flow.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace altair::live {

enum class ObiType : std::uint8_t { A, B, C, D };
inline constexpr ObiType kObiTypes[] = {ObiType::A, ObiType::B, ObiType::C, ObiType::D};

[[nodiscard]] inline const char* obi_type_letter(ObiType t) noexcept {
    switch (t) {
    case ObiType::A: return "A";
    case ObiType::B: return "B";
    case ObiType::C: return "C";
    case ObiType::D: return "D";
    }
    return "?";
}
[[nodiscard]] inline const char* obi_type_name(ObiType t) noexcept {
    switch (t) {
    case ObiType::A: return "bid pressure";
    case ObiType::B: return "ask pressure";
    case ObiType::C: return "absorption";
    case ObiType::D: return "withdrawal";
    }
    return "?";
}

struct ObiSignalRules {
    double obi_entry = 0.30;
    int min_persist = 3;                       ///< book updates (A, B)
    std::int64_t min_persist_ns = 500'000'000;
};

/// +1 long, -1 short, 0 none for `type` on `s`; `why` says what it saw, or
/// what is missing, with the numbers.
[[nodiscard]] inline int obi_signal(ObiType type, const flow::FlowSignals& s, const ObiSignalRules& r, std::string& why) {
    char b[240];
    if (!s.valid) { why = "no tradable book"; return 0; }
    switch (type) {
    case ObiType::A:
    case ObiType::B: {
        const int dir = type == ObiType::A ? 1 : -1;
        const double obi = s.obi_short * dir;
        const long long mine = dir > 0 ? s.buy_exec : s.sell_exec, theirs = dir > 0 ? s.sell_exec : s.buy_exec;
        const int run = s.persist * dir;
        const double secs = static_cast<double>(s.persist_ns) / 1e9;
        if (obi < r.obi_entry) {
            std::snprintf(b, sizeof b, "OBI5 %+.2f, needs %s%.2f", s.obi_short, dir > 0 ? ">= +" : "<= -", r.obi_entry);
            why = b;
            return 0;
        }
        if (run < r.min_persist || s.persist_ns < r.min_persist_ns) {
            std::snprintf(b, sizeof b, "OBI5 %+.2f not held yet (%d updates, %.1f s; needs %d and %.1f s)", s.obi_short, run < 0 ? 0 : run, secs,
                          r.min_persist, static_cast<double>(r.min_persist_ns) / 1e9);
            why = b;
            return 0;
        }
        if (mine <= theirs) {
            std::snprintf(b, sizeof b, "OBI5 %+.2f held, but %s %lld <= %lld", s.obi_short, dir > 0 ? "buying" : "selling", mine, theirs);
            why = b;
            return 0;
        }
        std::snprintf(b, sizeof b, "%s dominate: OBI5 %.2f held %d updates / %.1f s, %s %lld vs %lld", dir > 0 ? "bids" : "asks", s.obi_short, run,
                      secs, dir > 0 ? "buys" : "sells", mine, theirs);
        why = b;
        return dir;
    }
    case ObiType::C:
        if (s.absorption == 0) { why = "no absorption"; return 0; }
        if (s.absorption > 0)
            std::snprintf(b, sizeof b, "absorption: selling %lld into bids that refill (hidden %lld), the bid holds -- reversal up",
                          static_cast<long long>(s.sell_exec), static_cast<long long>(s.hidden_sell));
        else
            std::snprintf(b, sizeof b, "absorption: buying %lld into asks that refill (hidden %lld), the ask holds -- reversal down",
                          static_cast<long long>(s.buy_exec), static_cast<long long>(s.hidden_buy));
        why = b;
        return s.absorption;
    case ObiType::D:
        if (s.withdrawal == 0) { why = "no withdrawal"; return 0; }
        if (s.withdrawal > 0)
            std::snprintf(b, sizeof b, "withdrawal: asks pulled (%lld cancelled) while buyers lift them -- breakout up", static_cast<long long>(s.ask_cancel));
        else
            std::snprintf(b, sizeof b, "withdrawal: bids pulled (%lld cancelled) while sellers hit them -- breakout down", static_cast<long long>(s.bid_cancel));
        why = b;
        return s.withdrawal;
    }
    return 0;
}

} // namespace altair::live
