// Acceptance tests for server/quote_payload.hpp.
//
// The byte vector is the contract, as for the price frame: the expected hex
// below was produced by Python's struct module ('<IHH' + 14 x '<q'), not by
// this encoder, so a mistake made the same way in both directions still shows.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <server/quote_payload.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

altair::QuotePayload sample() {
    altair::QuotePayload q;
    q.token = 256265;
    q.flags = 0x7F;
    q.open = 2350000; q.high = 2365050; q.low = 2341000; q.prev_close = 2348025;
    q.bid = 2355000; q.ask = 2355050; q.bid_qty = 75; q.ask_qty = 150;
    q.avg_price = 2353310; q.total_buy = 1200000; q.total_sell = 980000;
    q.upper_circuit = 2582800; q.lower_circuit = 2113200;
    q.last_trade_ns = 1790000000123456789LL;
    return q;
}

}  // namespace

int main() {
    using namespace altair;
    std::printf("quote payload\n");

    std::uint8_t buf[kQuotePayloadBytes]{};
    const auto n = encode_quote(sample(), buf, sizeof buf);
    check(n.has_value() && *n == 120, "encodes to exactly 120 bytes");
    check(hex(buf, sizeof buf) ==
              "09e903007f000000b0db2300000000007a1624000000000088b8230000000000"
              "f9d323000000000038ef2300000000006aef2300000000004b00000000000000"
              "96000000000000009ee8230000000000804f12000000000020f40e0000000000"
              "1069270000000000b03e20000000000015cd4e2b845bd718",
          "bytes match the independently computed vector");

    const auto d = decode_quote(buf, sizeof buf);
    check(d.has_value() && d->token == 256265 && d->bid == 2355000 && d->ask_qty == 150
              && d->lower_circuit == 2113200 && d->last_trade_ns == 1790000000123456789LL,
          "decodes every field back");
    check(d.has_value() && d->has(kQuoteHasTop) && !d->has(kQuoteReplay), "presence bits survive");

    check(!decode_quote(buf, 119).has_value(), "a short buffer is refused");
    std::uint8_t bad[kQuotePayloadBytes];
    for (std::size_t i = 0; i < sizeof bad; ++i) { bad[i] = buf[i]; }
    bad[5] = 0x80;   // flags bit 15: unknown
    check(!decode_quote(bad, sizeof bad).has_value(), "an unknown flag bit is refused, not ignored");
    bad[5] = buf[5];
    bad[6] = 1;      // reserved must be zero
    check(!decode_quote(bad, sizeof bad).has_value(), "a non-zero reserved field is refused");

    QuotePayload unknown = sample();
    unknown.flags = 0x8000;
    check(!encode_quote(unknown, buf, sizeof buf).has_value(), "the encoder refuses an unknown flag");
    check(!encode_quote(sample(), buf, 100).has_value(), "the encoder refuses a short buffer");

    std::printf("%s\n", failures == 0 ? "all quote payload checks passed" : "quote payload checks did not pass");
    return failures == 0 ? 0 : 1;
}
