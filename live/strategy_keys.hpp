// live/strategy_keys.hpp -- a live model's name, as the LIVE switch and the
// order router know it.
//
// Every model the engine runs has a Paper/Live switch (the Terminal's LIVE
// dialog and the Models table). The switch is stored in data/live_trading.json
// under "strategies", by this key; the engine sends a model's real requests
// as "strategy.<key>" and the router finds its caps by the same key.
//
// The two strategies that had switches before keep their old keys
// (arbitrage, ohl), so an arm file written before stays valid. Every other
// model's key is its name in lower case, letters and digits, with a '-'
// between words: "OBI A futures BANKNIFTY" -> "obi-a-futures-banknifty".
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace altair::live {

/// The longest key the router accepts.
inline constexpr std::size_t kStrategyKeyMax = 64;

/// Keys that never send a real order, whatever the switch says. Option
/// arbitrage opens with option BUYS (conversions, reversals, boxes), and
/// ALTAIR never buys options (Smit, 2026-10-25).
inline constexpr std::string_view kPaperOnlyStrategyKeys[] = {"option-arbitrage"};

[[nodiscard]] inline bool strategy_paper_only(std::string_view key) noexcept {
    for (const std::string_view k : kPaperOnlyStrategyKeys)
        if (k == key) return true;
    return false;
}

/// A key is 1 to 64 of a-z, 0-9 and '-'.
[[nodiscard]] inline bool strategy_key_ok(std::string_view key) noexcept {
    if (key.empty() || key.size() > kStrategyKeyMax) return false;
    for (const char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

/// The key of a model, by its name. Empty for a name with no letter or digit.
[[nodiscard]] inline std::string strategy_key(std::string_view model) {
    if (model == "Cross-exchange arbitrage") return "arbitrage";
    if (model == "OHL" || model.substr(0, 4) == "OHL ") return "ohl";
    std::string k;
    bool gap = false;
    for (const char raw : model) {
        char c = raw;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!word) { gap = !k.empty(); continue; }
        if (gap) k.push_back('-');
        gap = false;
        k.push_back(c);
        if (k.size() >= kStrategyKeyMax) break;
    }
    while (!k.empty() && k.back() == '-') k.pop_back();
    return k;
}

} // namespace altair::live
