// Tests for app/fyers_price_frames.hpp: FYERS socket updates into the price
// bus's trade, quote and book frames.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/fyers_price_frames.hpp>

#include <array>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

altair::fyers_hsm::HsmUpdate update(altair::fyers_hsm::HsmTopic topic, const std::int32_t* v, std::size_t n) {
    altair::fyers_hsm::HsmUpdate u;
    u.topic = topic;
    u.value = v;
    u.present = n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
    u.precision = 2;
    u.multiplier = 1;
    return u;
}

}  // namespace

int main() {
    using namespace altair;
    using namespace altair::fyers_frames;
    using fyers_hsm::HsmTopic;
    std::printf("fyers price frames\n");
    const std::int64_t recv = 1'790'000'000'000'000'000LL;

    // A NIFTY future: ltp 24012.50, volume 1,234,500, LTT 1790000000 s, feed 1790000001 s.
    std::array<std::int32_t, 21> sf{2401250, 1234500, 1790000000, 1790000001, 650, 1300, 2401200, 2401300, 65,
                                    900000, 850000, 2398000, 1500000, 2390000, 2410000, 2500000, 2200000,
                                    2160000, 2640000, 2395000, 2393050};
    LastTrade last;
    Frames f;
    check(to_frames(update(HsmTopic::Scrip, sf.data(), sf.size()), 12468226u, last, recv, f), "a symbol update maps");
    check(f.trade && f.quote && !f.book, "the first update is a trade and a quote");
    check(f.price.token == 12468226u && f.price.last_paise == 2401250 && f.price.last_qty == 65
              && f.price.volume == 1234500 && f.price.oi == 1500000,
          "trade frame: last price, last quantity, volume and OI");
    check(f.price.has(kPriceHasVolume) && f.price.has(kPriceHasOi) && !f.price.has(kPriceNoExchTs),
          "presence bits set, and the exchange stamp is the exchange's");
    check(f.price.exchange_ts_ns == 1'790'000'000'000'000'000LL, "a trade is stamped at its last-trade time");
    const auto& q = f.quote_p;
    check(q.bid == 2401200 && q.ask == 2401300 && q.bid_qty == 650 && q.ask_qty == 1300 && q.has(kQuoteHasTop),
          "quote frame: best bid and ask with sizes");
    check(q.open == 2395000 && q.high == 2410000 && q.low == 2390000 && q.prev_close == 2393050
              && q.has(kQuoteHasOhlc) && q.has(kQuoteHasPrevClose),
          "quote frame: open, high, low, previous close");
    check(q.avg_price == 2398000 && q.total_buy == 900000 && q.total_sell == 850000 && q.upper_circuit == 2640000
              && q.lower_circuit == 2160000,
          "quote frame: average price, totals, circuits");
    check(f.quote_ns == 1'790'000'001'000'000'000LL, "a quote is stamped at the feed time");

    // A requote: only the bid moved. No trade.
    sf[6] = 2401150;
    check(to_frames(update(HsmTopic::Scrip, sf.data(), sf.size()), 12468226u, last, recv, f) && f.quote && !f.trade
              && f.quote_p.bid == 2401150,
          "a requote is a quote, not a trade -- the tape stays a tape of trades");
    // Same price, more volume: a trade at an unchanged price.
    sf[1] = 1234565;
    check(to_frames(update(HsmTopic::Scrip, sf.data(), sf.size()), 12468226u, last, recv, f) && f.trade
              && f.price.volume == 1234565,
          "a trade at an unchanged price is still a trade");

    // A price that is not a whole paisa is refused, not rounded.
    std::array<std::int32_t, 21> odd = sf;
    odd[0] = 240125;
    altair::fyers_hsm::HsmUpdate uo = update(HsmTopic::Scrip, odd.data(), odd.size());
    uo.precision = 3;   // 240.125 rupees
    LastTrade l2;
    check(to_frames(uo, 1u, l2, recv, f) && !f.trade, "a sub-paisa last price publishes no trade");

    // An index: ltp, prev close, feed time, high, low, open.
    std::array<std::int32_t, 6> idx{2394560, 2390000, 1790000002, 2399000, 2385000, 2391000};
    LastTrade li;
    check(to_frames(update(HsmTopic::Index, idx.data(), idx.size()), 256265u, li, recv, f) && f.trade && f.quote,
          "an index update maps to a trade and a quote");
    check(f.price.last_paise == 2394560 && !f.price.has(kPriceHasVolume) && !f.price.has(kPriceHasOi),
          "an index trade carries no volume and no OI");
    check(!f.quote_p.has(kQuoteHasTop) && f.quote_p.has(kQuoteHasOhlc) && f.quote_p.prev_close == 2390000,
          "an index quote has OHLC and a previous close, and no bid or ask");
    check(to_frames(update(HsmTopic::Index, idx.data(), idx.size()), 256265u, li, recv, f) && !f.trade,
          "an unchanged index value is not a second trade");

    // Depth: 3 complete levels, then a gap.
    std::array<std::int32_t, 30> dp{};
    for (int i = 0; i < 3; ++i) {
        dp[static_cast<std::size_t>(i)] = 2401200 - 10 * i;
        dp[static_cast<std::size_t>(5 + i)] = 2401300 + 10 * i;
        dp[static_cast<std::size_t>(10 + i)] = 65 * (i + 1);
        dp[static_cast<std::size_t>(15 + i)] = 130 * (i + 1);
        dp[static_cast<std::size_t>(20 + i)] = i + 1;
        dp[static_cast<std::size_t>(25 + i)] = i + 2;
    }
    altair::fyers_hsm::HsmUpdate ud = update(HsmTopic::Depth, dp.data(), dp.size());
    ud.present = 0;
    for (int i = 0; i < 3; ++i)
        for (int g = 0; g < 6; ++g) ud.present |= 1u << (5 * g + i);
    LastTrade ldp;
    check(to_frames(ud, 12468226u, ldp, recv, f) && f.book && !f.trade && !f.quote, "a depth update is a book frame");
    check(f.price.depth_levels == 3 && f.price.has(kPriceHasBook) && f.bids[2].price_paise == 2401180
              && f.asks[2].qty == 390 && f.asks[1].orders == 3,
          "three levels, prices, sizes and order counts");
    check(f.price.has(kPriceNoExchTs) && f.price.exchange_ts_ns == recv, "a book is stamped at receipt, and says so");

    std::printf("%s\n", failures == 0 ? "all fyers price frame checks passed" : "fyers price frame checks did not pass");
    return failures == 0 ? 0 : 1;
}
