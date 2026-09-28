#include <feed/fyers_decoder.hpp>

#include <cstdio>
#include <cstring>

using namespace altair;

int main() {
    static SpecStore store;
    ContractSpec spec{};
    spec.id = InstrumentId::Invalid;
    spec.token[static_cast<std::size_t>(FeedSource::Fyers)] = 123;
    std::memcpy(spec.symbol, "NSE:SBIN-EQ", 12);
    std::memcpy(spec.underlying, "SBIN", 5);
    spec.lot_size = LotSize{1};
    spec.tick_size = Price{5};
    spec.price_scale = 100;
    spec.valid_from = Timestamp{1};
    spec.valid_to = Timestamp::max();
    spec.snapshot_at = Timestamp{1};
    spec.source_hash = 1;
    const auto id = store.add(spec);
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    check(id.has_value(), "FYERS token is registered in the canonical spec store");
    Tick ticks[1]{};
    DepthUpdate depths[1]{};
    std::uint32_t seq = 10;
    const auto sf = decode_fyers_message(
        R"({"type":"sf","symbol":"NSE:SBIN-EQ","fyToken":"123","ltp":250.05,"last_traded_qty":7,"vol_traded_today":1000,"last_traded_time":1700000000})",
        store, Timestamp{2'000'000'000}, seq, ticks, 1, depths, 1);
    check(sf.has_value() && sf->ticks == 1, "SymbolUpdate decodes into one normalized tick");
    if (sf) {
        check(ticks[0].id == *id && ticks[0].source == FeedSource::Fyers,
              "FYERS identity and source survive normalization");
        check(ticks[0].last.raw() == 25005 && ticks[0].last_qty.raw() == 7,
              "decimal LTP is exact paise and trade quantity is preserved");
        check(!has_flag(ticks[0].flags, TickFlag::NoExchangeTs)
              && ticks[0].exchange_ts.ns_since_epoch() == 1'700'000'000'000'000'000LL,
              "the exchange timestamp is used when FYERS supplies it");
    }
    const auto dp = decode_fyers_message(
        R"({"type":"dp","fyToken":123,"bid_price1":250.00,"ask_price1":250.05,"bid_size1":10,"ask_size1":12,"bid_order1":2,"ask_order1":3,"bid_price2":249.95,"ask_price2":250.10,"bid_size2":8,"ask_size2":9})",
        store, Timestamp{3'000'000'000}, seq, ticks, 1, depths, 1);
    check(dp.has_value() && dp->depths == 1 && depths[0].bid_levels == 2,
          "DepthUpdate decodes bounded real levels");
    check(depths[0].bid[0].px.raw() == 25000
              && depths[0].ask[0].orders == 3,
          "depth prices, quantities and order counts are exact");
    const auto unknown = decode_fyers_message(
        R"({"type":"sf","fyToken":999,"ltp":1.0})", store,
        Timestamp{4}, seq, ticks, 1, depths, 1);
    check(unknown.has_value() && unknown->unknown_token == 1,
          "an unmapped FYERS token is counted and never fabricated");
    const auto bad = decode_fyers_message(
        R"({"type":"sf","fyToken":123,"ltp":250.001})", store,
        Timestamp{4}, seq, ticks, 1, depths, 1);
    check(!bad.has_value() && bad.error() == FyersDecodeError::InvalidPrice,
          "sub-paise prices are refused rather than rounded");
    const auto unknown_type = decode_fyers_message(
        R"({"type":"heartbeat","fyToken":123,"ltp":250.05})", store,
        Timestamp{4}, seq, ticks, 1, depths, 1);
    check(!unknown_type.has_value()
              && unknown_type.error() == FyersDecodeError::Malformed,
          "unknown FYERS message types are refused, never treated as ticks");
    const auto huge_timestamp = decode_fyers_message(
        R"({"type":"sf","fyToken":123,"ltp":250.05,"last_traded_time":9223372037})",
        store, Timestamp{4}, seq, ticks, 1, depths, 1);
    check(huge_timestamp.has_value() && huge_timestamp->ticks == 1
              && has_flag(ticks[0].flags, TickFlag::NoExchangeTs),
          "an overflowing exchange timestamp falls back to receive time");
    const auto negative_ltp = decode_fyers_message(
        R"({"type":"sf","fyToken":123,"ltp":-1.0})", store,
        Timestamp{4}, seq, ticks, 1, depths, 1);
    check(!negative_ltp.has_value()
              && negative_ltp.error() == FyersDecodeError::InvalidPrice,
          "negative market prices are refused");
    return failures == 0 ? 0 : 1;
}
