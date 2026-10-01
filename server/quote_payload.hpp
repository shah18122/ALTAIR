// server/quote_payload.hpp -- the rest of a market-watch row.
//
// price_payload.hpp carries what changes on every trade: last price, last
// quantity, volume, open interest, and (on its own topic) the five-level book.
// A trading terminal's market watch shows more than that on every row: the
// day's open, high and low, the previous close the change is measured from,
// the best bid and ask with their sizes, the average traded price, the total
// quantities waiting to buy and sell, the circuit limits and the time of the
// last trade. FYERS' SymbolUpdate delivers all of them on every update; this
// is the frame that carries them.
//
// A SEPARATE TOPIC, NOT A LONGER PRICE FRAME. The price frame has conformance
// vectors and readers that rely on its length. A quote is a different message
// on topic 3; a reader that does not know topic 3 must skip it by topic, and
// desktop/price_client.hpp does.
//
// ABSENCE IS NOT ZERO, the same rule as the price frame: an index has no bid,
// no ask and no circuit limits, and every group of fields has a presence bit.
//
// Wire: little-endian, fixed width, 120 bytes.
//   0 u32 token        4 u16 flags         6 u16 reserved (0)
//   8 i64 open        16 i64 high         24 i64 low          32 i64 prev_close
//  40 i64 bid         48 i64 ask          56 i64 bid_qty      64 i64 ask_qty
//  72 i64 avg_price   80 i64 total_buy    88 i64 total_sell
//  96 i64 upper_circuit                  104 i64 lower_circuit
// 112 i64 last_trade_ns
// Prices are paise.

#pragma once

#include <server/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::uint32_t kTopicQuote = 3;

enum QuoteFlags : std::uint16_t {
    kQuoteHasOhlc      = 1u << 0,   ///< open, high, low
    kQuoteHasPrevClose = 1u << 1,
    kQuoteHasTop       = 1u << 2,   ///< bid, ask and their sizes
    kQuoteHasAtp       = 1u << 3,
    kQuoteHasTotals    = 1u << 4,   ///< total buy and sell quantity
    kQuoteHasCircuit   = 1u << 5,
    kQuoteHasLtt       = 1u << 6,
    kQuoteReplay       = 1u << 7,   ///< same meaning as kPriceReplay
    kQuoteSimulated    = 1u << 8    ///< same meaning as kPriceSimulated
};
inline constexpr std::uint16_t kQuoteKnownFlags = 0x01FF;

struct QuotePayload {
    std::uint32_t token = 0;
    std::uint16_t flags = 0;
    std::int64_t open = 0, high = 0, low = 0, prev_close = 0;
    std::int64_t bid = 0, ask = 0, bid_qty = 0, ask_qty = 0;
    std::int64_t avg_price = 0, total_buy = 0, total_sell = 0;
    std::int64_t upper_circuit = 0, lower_circuit = 0;
    std::int64_t last_trade_ns = 0;

    [[nodiscard]] bool has(QuoteFlags f) const noexcept {
        return (flags & static_cast<std::uint16_t>(f)) != 0;
    }
};

inline constexpr std::size_t kQuotePayloadBytes = 120;

[[nodiscard]] inline std::expected<std::size_t, ProtocolError>
encode_quote(const QuotePayload& q, std::uint8_t* out, std::size_t cap) noexcept {
    if ((q.flags & ~kQuoteKnownFlags) != 0) {
        // RULE 11: refuse. An unknown bit is a field this version cannot
        // describe; sending it would let a reader trust a value nobody set.
        return std::unexpected(ProtocolError::Unspecified);
    }
    if (out == nullptr || cap < kQuotePayloadBytes) { return std::unexpected(ProtocolError::ShortBuffer); }
    wire::put_u32(out + 0, q.token);
    wire::put_u16(out + 4, q.flags);
    wire::put_u16(out + 6, 0);
    const std::int64_t v[14] = {q.open, q.high, q.low, q.prev_close, q.bid, q.ask, q.bid_qty, q.ask_qty,
                                q.avg_price, q.total_buy, q.total_sell, q.upper_circuit, q.lower_circuit,
                                q.last_trade_ns};
    for (std::size_t i = 0; i < 14; ++i) { wire::put_i64(out + 8 + 8 * i, v[i]); }
    return kQuotePayloadBytes;
}

[[nodiscard]] inline std::expected<QuotePayload, ProtocolError>
decode_quote(const std::uint8_t* in, std::size_t n) noexcept {
    if (in == nullptr || n < kQuotePayloadBytes) { return std::unexpected(ProtocolError::ShortBuffer); }
    QuotePayload q;
    q.token = wire::get_u32(in + 0);
    q.flags = wire::get_u16(in + 4);
    if ((q.flags & ~kQuoteKnownFlags) != 0 || wire::get_u16(in + 6) != 0) {
        return std::unexpected(ProtocolError::Unspecified);
    }
    std::int64_t* v[14] = {&q.open, &q.high, &q.low, &q.prev_close, &q.bid, &q.ask, &q.bid_qty, &q.ask_qty,
                           &q.avg_price, &q.total_buy, &q.total_sell, &q.upper_circuit, &q.lower_circuit,
                           &q.last_trade_ns};
    for (std::size_t i = 0; i < 14; ++i) { *v[i] = wire::get_i64(in + 8 + 8 * i); }
    return q;
}

}  // namespace altair
