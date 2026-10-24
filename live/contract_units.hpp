// live/contract_units.hpp -- what one lot of an MCX or currency contract is,
// which the Kite master does not say, and when those markets close.
//
// LOTS. The Kite master gives lot_size 1 for every MCX and currency contract
// (an order there is in lots). The paper book counts units -- an NFO lot of
// 75 is 75 units -- so P&L is quantity x price move. These are the units one
// lot moves by per rupee of quoted price, from the MCX and NSE contract
// specifications as of 2026-10: CRUDEOIL is 100 barrels quoted per barrel
// (100), GOLD 1 kg quoted per 10 g (100), SILVER 30 kg quoted per kg (30).
// UNVERIFIED here: the exchanges revise lot sizes. The selection line shows
// "lot N"; check it against the exchange's contract page before trusting
// P&L. An underlying not listed is not streamed (RULE 11: refused, not
// guessed).
//
// CURRENCY. INR pairs only (USDINR, EURINR, GBPINR, JPYINR); cross pairs settle
// in a foreign currency and interest-rate futures share the segment, neither
// is here. Their quotes have four decimals (tick Rs 0.0025) and the price
// wire carries whole paise (instruments/contract_spec.hpp: a currency scale is
// not representable), so they are QUOTED PER 100 UNITS of the base: USDINR at
// 83.4525 is 8,345.25 rupees per 100 USD, exactly, and one lot (1,000 USD;
// JPYINR: 100,000 JPY quoted per 100) is 10 of those. Strikes and ticks are
// scaled the same way, so orders, books and P&L all agree.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace altair::live {

/// Units one lot moves by per rupee of quoted price, by MCX underlying.
[[nodiscard]] inline std::optional<std::int64_t> mcx_lot_units(std::string_view underlying) noexcept {
    static constexpr std::array<std::pair<std::string_view, std::int64_t>, 21> kUnits{{
        {"CRUDEOIL", 100},     // 100 barrels, per barrel
        {"CRUDEOILM", 10},     // 10 barrels
        {"NATURALGAS", 1250},  // 1,250 mmBtu, per mmBtu
        {"NATGASMINI", 250},   // 250 mmBtu
        {"GOLD", 100},         // 1 kg, per 10 g
        {"GOLDM", 10},         // 100 g, per 10 g
        {"GOLDTEN", 1},        // 10 g, per 10 g
        {"GOLDGUINEA", 1},     // 8 g, per 8 g
        {"GOLDPETAL", 1},      // 1 g, per g
        {"SILVER", 30},        // 30 kg, per kg
        {"SILVERM", 5},        // 5 kg
        {"SILVERMIC", 1},      // 1 kg
        {"COPPER", 2500},      // 2,500 kg, per kg
        {"ZINC", 5000},        // 5 t, per kg
        {"ZINCMINI", 1000},    // 1 t
        {"LEAD", 5000},
        {"LEADMINI", 1000},
        {"ALUMINIUM", 5000},
        {"ALUMINI", 1000},
        {"NICKEL", 1500},      // 1.5 t, per kg
        {"MENTHAOIL", 360},    // 360 kg, per kg
    }};
    for (const auto& [name, units] : kUnits)
        if (name == underlying) return units;
    return std::nullopt;
}

/// The INR currency pairs that stream (see the header).
[[nodiscard]] inline bool inr_currency_pair(std::string_view underlying) noexcept {
    return underlying == "USDINR" || underlying == "EURINR" || underlying == "GBPINR" || underlying == "JPYINR";
}

/// Currency is quoted per this many units of the base currency.
inline constexpr std::int64_t kCurrencyQuotePer = 100;
/// One currency lot in those quote units: 1,000 USD / EUR / GBP (100,000 JPY
/// quoted per 100) is 10 x 100.
inline constexpr std::int64_t kCurrencyLotQuotes = 10;

/// The market-watch groups the price service gives these segments.
inline constexpr std::string_view kCommodityGroup = "Commodity";
inline constexpr std::string_view kCurrencyGroup = "Currency";

/// When the segment's session ends, IST minutes after midnight, plus five so
/// the last ticks land: MCX trades to 23:30 (23:55 while the US is on winter
/// time, so 23:55 is used all year), currency to 17:00.
inline constexpr int kCommodityCloseMinute = 23 * 60 + 55 + 4;   // 23:59
inline constexpr int kCurrencyCloseMinute = 17 * 60 + 5;

/// The latest close the streamed groups need, or `base` when later.
template <class Range, class GroupOf>
[[nodiscard]] int session_end_minute(const Range& instruments, GroupOf group_of, int base) {
    int end = base;
    for (const auto& in : instruments) {
        const std::string_view g = group_of(in);
        if (g == kCommodityGroup && end < kCommodityCloseMinute) end = kCommodityCloseMinute;
        if (g == kCurrencyGroup && end < kCurrencyCloseMinute) end = kCurrencyCloseMinute;
    }
    return end;
}

} // namespace altair::live
