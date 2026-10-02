// app/kite_price_frames.hpp -- the market-watch fields of a Kite ticker frame.
//
// feed/kite_decoder.hpp turns a Kite binary frame into Ticks (last price,
// quantity, volume, OI) and DepthUpdates (five levels a side): exactly what an
// engine needs and no more. A market watch needs the rest of what Kite's quote
// and full packets carry -- open, high, low, the previous close the change is
// measured from, the average traded price, the total quantities waiting to
// buy and sell, the best bid and ask -- and that is the price bus's QUOTE
// frame (server/quote_payload.hpp). This walks the same frame and builds one
// quote per packet, so altair_price_service --kite fills the same columns the
// FYERS feed does.
//
// Layout (Zerodha's documentation; big-endian; prices in paise for every
// segment but currency, which is skipped exactly as the decoder skips it):
//   tradable quote (44)/full (184): 0 token  4 ltp  8 ltq  12 avg  16 volume
//       20 total buy  24 total sell  28 open  32 high  36 low  40 close
//       full only: 44 last-trade time (s)  48 oi ... 60 exchange time (s)
//       64 five buy levels then 124 five sell levels, 12 bytes each
//       (u32 qty, u32 price, u16 orders, u16 pad)
//   index quote (28)/full (32): 0 token  4 ltp  8 high  12 low  16 open
//       20 close  24 change  (full) 28 exchange time (s)
// `close` is the PREVIOUS session's close in both, which is what a change
// column is measured from.

#pragma once

#include <feed/kite_decoder.hpp>
#include <server/quote_payload.hpp>

#include <cstddef>
#include <cstdint>

namespace altair::kite_frames {

/// Calls `emit(const QuotePayload&, std::int64_t ns)` once per quote or full
/// packet. Returns the number emitted. A frame whose framing is broken stops
/// at the break; packets before it are kept (they were whole).
template <class Emit>
std::size_t quotes_from_frame(const std::uint8_t* buf, std::size_t len, std::int64_t recv_ns, Emit&& emit) {
    namespace d = altair::detail;
    if (buf == nullptr || len < 2) return 0;
    const std::size_t count = d::be16(buf);
    std::size_t pos = 2, emitted = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos + 2 > len) break;
        const std::size_t plen = d::be16(buf + pos);
        pos += 2;
        if (pos + plen > len) break;
        const std::uint8_t* p = buf + pos;
        pos += plen;
        const bool tradable = plen == d::kKiteQuoteLen || plen == d::kKiteFullLen;
        const bool index = plen == d::kKiteIndexQuoteLen || plen == d::kKiteIndexFullLen;
        if (!tradable && !index) continue;   // LTP-only packets carry no quote
        const std::uint32_t token = d::be32(p);
        if (kite_price_scale(kite_segment_of(token)) != 100) continue;
        const auto px = [p](std::size_t off) { return static_cast<std::int64_t>(d::be32(p + off)); };

        QuotePayload q;
        q.token = token;
        std::int64_t ns = recv_ns;
        if (tradable) {
            q.open = px(28); q.high = px(32); q.low = px(36); q.prev_close = px(40);
            q.avg_price = px(12);
            q.total_buy = px(20); q.total_sell = px(24);
            q.flags = kQuoteHasOhlc | kQuoteHasPrevClose | kQuoteHasAtp | kQuoteHasTotals;
            if (plen == d::kKiteFullLen) {
                const std::int64_t ltt = px(44);
                if (ltt > 0) { q.last_trade_ns = ltt * 1'000'000'000LL; q.flags |= kQuoteHasLtt; }
                const std::int64_t ets = px(60);
                if (ets > 0) ns = ets * 1'000'000'000LL;
                const std::uint8_t* b0 = p + d::kKiteBuyDepthOff;
                const std::uint8_t* a0 = p + d::kKiteSellDepthOff;
                q.bid_qty = static_cast<std::int64_t>(d::be32(b0));
                q.bid = static_cast<std::int64_t>(d::be32(b0 + 4));
                q.ask_qty = static_cast<std::int64_t>(d::be32(a0));
                q.ask = static_cast<std::int64_t>(d::be32(a0 + 4));
                // An empty side reads as zero on the wire; absence is not zero.
                if (q.bid > 0 || q.ask > 0) q.flags |= kQuoteHasTop;
            }
        } else {
            q.high = px(8); q.low = px(12); q.open = px(16); q.prev_close = px(20);
            q.flags = kQuoteHasOhlc | kQuoteHasPrevClose;
            if (plen == d::kKiteIndexFullLen) {
                const std::int64_t ets = px(28);
                if (ets > 0) ns = ets * 1'000'000'000LL;
            }
        }
        // A packet before the open can carry zeros; a zero OHLC is absence.
        if (q.open <= 0 && q.high <= 0 && q.low <= 0) q.flags &= static_cast<std::uint16_t>(~kQuoteHasOhlc);
        if (q.prev_close <= 0) q.flags &= static_cast<std::uint16_t>(~kQuoteHasPrevClose);
        emit(q, ns);
        ++emitted;
    }
    return emitted;
}

} // namespace altair::kite_frames
