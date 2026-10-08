// app/fyers_price_frames.hpp -- one FYERS socket update into price-bus frames.
//
// feed/fyers_hsm.hpp decodes the FYERS data socket into HsmUpdate: the full
// current state of one topic, as raw integers in the SDK's field order. The
// price service publishes three kinds of frame (server/price_payload.hpp,
// server/quote_payload.hpp), and this is the mapping, kept out of the
// service's main so it is tested without a socket:
//
//   symbol update (sf)  -> a QUOTE frame every time (OHLC, previous close,
//                          best bid/ask and sizes, ATP, totals, circuits,
//                          last-trade time), and a TRADE frame when the last
//                          price, the day's volume or the last-trade time
//                          moved -- i.e. when something actually traded, so
//                          the time-and-sales tape is a tape of trades, not of
//                          requotes;
//   index update (if)   -> a QUOTE frame, and a TRADE frame when the value moved;
//   depth update (dp)   -> a BOOK frame, 1-5 levels a side.
//
// Prices are converted exactly (fyers_hsm::hsm_paise); a price that is not a
// whole number of paise is refused, not rounded. Time: the exchange's own
// stamps (last-trade time for trades, feed time for quotes); with neither, the
// receive time, flagged kPriceNoExchTs.

#pragma once

#include <feed/fyers_hsm.hpp>
#include <server/price_payload.hpp>
#include <server/quote_payload.hpp>

#include <cstdint>

namespace altair::fyers_frames {

/// What was last published as a trade, per token: the trade test.
struct LastTrade {
    std::int64_t ltp = -1, volume = -1, ltt = -1;
};

struct Frames {
    bool trade = false, quote = false, book = false;
    PricePayload price{};     ///< the trade frame, or the book frame's header
    QuotePayload quote_p{};
    PriceLevel bids[kMaxDepthLevels]{};
    PriceLevel asks[kMaxDepthLevels]{};
    std::int64_t trade_ns = 0, quote_ns = 0;
};

namespace detail {
inline bool paise(const fyers_hsm::HsmUpdate& u, std::size_t i, std::int64_t& out) noexcept {
    if (!u.has(i)) return false;
    Price p{};
    if (!fyers_hsm::hsm_paise(u.value[i], u.precision, u.multiplier, p)) return false;
    out = p.raw();
    return true;
}
inline bool count(const fyers_hsm::HsmUpdate& u, std::size_t i, std::int64_t& out) noexcept {
    if (!u.has(i) || u.value[i] < 0) return false;
    out = u.value[i];
    return true;
}
} // namespace detail

/// Map one update for `token` (a Kite instrument token). `recv_ns`: when it
/// arrived. Returns false when nothing could be taken from it.
[[nodiscard]] inline bool to_frames(const fyers_hsm::HsmUpdate& u, std::uint32_t token, LastTrade& last,
                                    std::int64_t recv_ns, Frames& f) noexcept {
    using fyers_hsm::HsmTopic;
    f = Frames{};
    if (u.value == nullptr) return false;

    if (u.topic == HsmTopic::Depth) {
        // FIVE levels: the HSM depth topic's 30 fields are five bid prices,
        // five ask prices, five of each size and five of each order count.
        // Looping to kMaxDepthLevels (50, the TBT book's size) read field 5+i
        // as "level 6" -- ask price 1 shown as a bid, bid size 1 as an ask
        // price -- which is the garbage below level 5 the Terminal showed.
        std::uint16_t levels = 0;
        for (std::size_t i = 0; i < altair::kDepthLevels; ++i) {
            std::int64_t bp = 0, ap = 0, bq = 0, aq = 0;
            if (!detail::paise(u, i, bp) || !detail::paise(u, 5 + i, ap) || !detail::count(u, 10 + i, bq)
                || !detail::count(u, 15 + i, aq))
                break;
            f.bids[i] = PriceLevel{bp, bq, u.has(20 + i) && u.value[20 + i] >= 0 ? static_cast<std::uint32_t>(u.value[20 + i]) : 0u, 0u};
            f.asks[i] = PriceLevel{ap, aq, u.has(25 + i) && u.value[25 + i] >= 0 ? static_cast<std::uint32_t>(u.value[25 + i]) : 0u, 0u};
            ++levels;
        }
        if (levels == 0) return false;
        f.book = true;
        f.price.token = token;
        f.price.flags = static_cast<std::uint16_t>(kPriceHasBook | kPriceNoExchTs);   // a depth update carries no time
        f.price.depth_levels = levels;
        f.price.exchange_ts_ns = recv_ns;
        f.trade_ns = recv_ns;
        return true;
    }

    auto& q = f.quote_p;
    q.token = token;
    std::int64_t ltp = 0;
    const bool has_ltp = detail::paise(u, 0, ltp) && ltp > 0;

    if (u.topic == HsmTopic::Index) {
        // kIndexFields: ltp, prev_close_price, exch_feed_time, high_price, low_price, open_price
        if (detail::paise(u, 1, q.prev_close) && q.prev_close > 0) q.flags |= kQuoteHasPrevClose;
        if (detail::paise(u, 5, q.open) && detail::paise(u, 3, q.high) && detail::paise(u, 4, q.low) && q.open > 0)
            q.flags |= kQuoteHasOhlc;
        const std::int64_t feed_ns = u.has(2) && u.value[2] > 0 ? static_cast<std::int64_t>(u.value[2]) * 1'000'000'000LL : 0;
        f.quote_ns = feed_ns > 0 ? feed_ns : recv_ns;
        f.quote = true;
        if (has_ltp && ltp != last.ltp) {
            f.trade = true;
            f.price.token = token;
            f.price.last_paise = ltp;
            f.price.exchange_ts_ns = f.quote_ns;
            if (feed_ns == 0) f.price.flags |= kPriceNoExchTs;
            f.trade_ns = f.quote_ns;
            last.ltp = ltp;
        }
        return true;
    }

    // kScripFields: 0 ltp, 1 vol_traded_today, 2 last_traded_time, 3 exch_feed_time,
    // 4 bid_size, 5 ask_size, 6 bid_price, 7 ask_price, 8 last_traded_qty, 9 tot_buy_qty,
    // 10 tot_sell_qty, 11 avg_trade_price, 12 OI, 13 low, 14 high, 15 Yhigh, 16 Ylow,
    // 17 lower_ckt, 18 upper_ckt, 19 open, 20 prev_close
    if (detail::paise(u, 20, q.prev_close) && q.prev_close > 0) q.flags |= kQuoteHasPrevClose;
    if (detail::paise(u, 19, q.open) && detail::paise(u, 14, q.high) && detail::paise(u, 13, q.low) && q.open > 0)
        q.flags |= kQuoteHasOhlc;
    if (detail::paise(u, 6, q.bid) && detail::paise(u, 7, q.ask) && detail::count(u, 4, q.bid_qty)
        && detail::count(u, 5, q.ask_qty))
        q.flags |= kQuoteHasTop;
    if (detail::paise(u, 11, q.avg_price) && q.avg_price > 0) q.flags |= kQuoteHasAtp;
    if (detail::count(u, 9, q.total_buy) && detail::count(u, 10, q.total_sell)) q.flags |= kQuoteHasTotals;
    if (detail::paise(u, 18, q.upper_circuit) && detail::paise(u, 17, q.lower_circuit) && q.upper_circuit > 0)
        q.flags |= kQuoteHasCircuit;
    const std::int64_t ltt_s = u.has(2) && u.value[2] > 0 ? u.value[2] : 0;
    if (ltt_s > 0) { q.flags |= kQuoteHasLtt; q.last_trade_ns = ltt_s * 1'000'000'000LL; }
    const std::int64_t feed_ns = u.has(3) && u.value[3] > 0 ? static_cast<std::int64_t>(u.value[3]) * 1'000'000'000LL : 0;
    f.quote_ns = feed_ns > 0 ? feed_ns : recv_ns;
    f.quote = true;

    std::int64_t volume = -1;
    const bool has_volume = detail::count(u, 1, volume);
    const bool moved = has_ltp && (ltp != last.ltp || (has_volume && volume != last.volume) || ltt_s != last.ltt);
    if (moved) {
        f.trade = true;
        auto& p = f.price;
        p.token = token;
        p.last_paise = ltp;
        std::int64_t lq = 0;
        if (detail::count(u, 8, lq)) p.last_qty = lq;
        if (has_volume) { p.flags |= kPriceHasVolume; p.volume = volume; }
        std::int64_t oi = 0;
        if (detail::count(u, 12, oi) && oi > 0) { p.flags |= kPriceHasOi; p.oi = oi; }
        const std::int64_t ts = ltt_s > 0 ? ltt_s * 1'000'000'000LL : feed_ns;
        if (ts > 0) { p.exchange_ts_ns = ts; } else { p.exchange_ts_ns = recv_ns; p.flags |= kPriceNoExchTs; }
        f.trade_ns = p.exchange_ts_ns;
        last.ltp = ltp;
        last.volume = has_volume ? volume : last.volume;
        last.ltt = ltt_s;
    }
    return true;
}

} // namespace altair::fyers_frames
