// Kite ticker frames into quote frames, offline: a full tradable packet, an
// index full packet, an LTP-only packet that carries no quote, a currency
// packet that is skipped, and a frame cut short.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/kite_price_frames.hpp>

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

void be16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 8);
    b[at + 1] = static_cast<std::uint8_t>(v);
}
void be32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (24 - 8 * k));
}

/// Append one packet of `len` bytes; returns its offset.
std::size_t packet(std::vector<std::uint8_t>& f, std::size_t len) {
    const std::size_t at = f.size();
    f.resize(at + 2 + len, 0);
    be16(f, at, static_cast<std::uint16_t>(len));
    return at + 2;
}

}  // namespace

int main() {
    std::printf("kite price frames\n");
    std::vector<std::uint8_t> f(2, 0);
    be16(f, 0, 4);

    const std::size_t fut = packet(f, 184);        // NIFTY future, NFO (segment 2)
    be32(f, fut + 0, 12468226u);
    be32(f, fut + 4, 2510050);    // ltp 25,100.50
    be32(f, fut + 12, 2505025);   // avg
    be32(f, fut + 20, 900000);    // total buy
    be32(f, fut + 24, 750000);    // total sell
    be32(f, fut + 28, 2490000);   // open
    be32(f, fut + 32, 2515000);   // high
    be32(f, fut + 36, 2485000);   // low
    be32(f, fut + 40, 2488000);   // previous close
    be32(f, fut + 44, 1790000000);   // last trade, s
    be32(f, fut + 60, 1790000001);   // exchange time, s
    be32(f, fut + 64, 150);       // best bid qty
    be32(f, fut + 68, 2510000);   // best bid
    be32(f, fut + 124, 75);       // best ask qty
    be32(f, fut + 128, 2510100);  // best ask

    const std::size_t idx = packet(f, 32);         // NIFTY 50 index (segment 9)
    be32(f, idx + 0, 256265u);
    be32(f, idx + 4, 2500000);
    be32(f, idx + 8, 2505000);    // high
    be32(f, idx + 12, 2470000);   // low
    be32(f, idx + 16, 2480000);   // open
    be32(f, idx + 20, 2475000);   // previous close
    be32(f, idx + 28, 1790000002);

    const std::size_t ltp = packet(f, 8);          // LTP mode: no quote
    be32(f, ltp + 0, 738561u);
    be32(f, ltp + 4, 300000);

    const std::size_t cds = packet(f, 44);         // currency (segment 3): skipped
    be32(f, cds + 0, 0x00001203u);

    std::vector<altair::QuotePayload> got;
    std::vector<std::int64_t> at_ns;
    const std::size_t n = altair::kite_frames::quotes_from_frame(
        f.data(), f.size(), 42, [&](const altair::QuotePayload& q, std::int64_t ns) {
            got.push_back(q);
            at_ns.push_back(ns);
        });
    check(n == 2 && got.size() == 2, "one quote per quote/full packet; LTP and currency packets give none");
    if (got.size() == 2) {
        const auto& q = got[0];
        check(q.token == 12468226u && q.open == 2490000 && q.high == 2515000 && q.low == 2485000
                  && q.prev_close == 2488000,
              "future: OHLC and the previous close, in paise");
        check(q.avg_price == 2505025 && q.total_buy == 900000 && q.total_sell == 750000,
              "future: average price and the totals waiting");
        check(q.has(altair::kQuoteHasTop) && q.bid == 2510000 && q.bid_qty == 150 && q.ask == 2510100
                  && q.ask_qty == 75,
              "future: best bid and ask from the first depth level");
        check(q.has(altair::kQuoteHasLtt) && q.last_trade_ns == 1790000000LL * 1'000'000'000LL
                  && at_ns[0] == 1790000001LL * 1'000'000'000LL,
              "future: last-trade time, stamped at the exchange time");
        const auto& i = got[1];
        check(i.token == 256265u && i.open == 2480000 && i.high == 2505000 && i.low == 2470000
                  && i.prev_close == 2475000 && !i.has(altair::kQuoteHasTop) && !i.has(altair::kQuoteHasAtp),
              "index: OHLC and previous close, and no book or ATP claimed");
    }

    // Cut inside the second packet: the first is kept, nothing past the break.
    std::size_t cut_n = altair::kite_frames::quotes_from_frame(f.data(), idx + 10, 0,
                                                               [](const altair::QuotePayload&, std::int64_t) {});
    check(cut_n == 1, "a frame cut short keeps the whole packets before the cut");

    std::vector<std::uint8_t> pre(2 + 2 + 44, 0);
    be16(pre, 0, 1);
    be16(pre, 2, 44);
    be32(pre, 4, 408065u);   // NSE equity, before the open: every field zero
    altair::QuotePayload z;
    altair::kite_frames::quotes_from_frame(pre.data(), pre.size(), 7, [&z](const altair::QuotePayload& q, std::int64_t) { z = q; });
    check(!z.has(altair::kQuoteHasOhlc) && !z.has(altair::kQuoteHasPrevClose),
          "zeros before the open are absence, not prices");

    std::printf("%s\n", failures == 0 ? "all kite price frame checks passed" : "kite price frame checks did not pass");
    return failures == 0 ? 0 : 1;
}
