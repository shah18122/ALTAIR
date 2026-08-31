// P2-02 acceptance tests for feed/kite_decoder.hpp.
//
// Every packet here is built byte by byte from the format documented in
// prompts/P2-02_kite_decoder.md §1, which was read from Zerodha's own client.
// No live data is needed and none was used.
//
// No check description here may contain the substring FAIL.

#include <feed/kite_decoder.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

namespace {

SpecStore g_store;

constexpr Timestamp kRecv{1787888700123456789LL};

// Tokens. The LOW BYTE is the segment: 2 = NseFO, 3 = NseCD, 9 = Indices.
constexpr std::uint32_t kFoToken    = 0x00AB'CD02u;   // NseFO
constexpr std::uint32_t kCdToken    = 0x00AB'CD03u;   // NseCD -- must be refused
constexpr std::uint32_t kIndexToken = 256265u;        // NIFTY 50; 256265 & 255 == 9
constexpr std::uint32_t kBlockedTok = 0x00AB'CE02u;
constexpr std::uint32_t kAbsentTok  = 0x00AB'CF02u;

// ── byte building ────────────────────────────────────────────────────────
struct Buf {
    // Ten full-mode packets in one frame is 2 + 10*(2+184) = 1862 bytes, so
    // 1024 was not enough and the benchmark walked off the end of the array.
    // It segfaulted rather than corrupting quietly, which was luck.
    static constexpr std::size_t kCap = 8192;
    std::uint8_t b[kCap]{};
    std::size_t n = 0;
    void u8(std::uint8_t v) {
        if (n >= kCap) {          // never silently overflow again
            std::printf("  FAIL: test buffer overflow at %zu bytes\n", n);
            ++failures;
            return;
        }
        b[n++] = v;
    }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v >> 8));
                                u8(static_cast<std::uint8_t>(v)); }
    void u32(std::uint32_t v) { u8(static_cast<std::uint8_t>(v >> 24));
                                u8(static_cast<std::uint8_t>(v >> 16));
                                u8(static_cast<std::uint8_t>(v >> 8));
                                u8(static_cast<std::uint8_t>(v)); }
};

/// A 184-byte full-mode packet. Depth levels are (qty, price, orders) triples;
/// pass fewer than five and the rest are zero-padded, exactly as Kite does.
void put_full(Buf& f, std::uint32_t token, std::uint32_t last, std::uint32_t lastq,
              std::uint32_t vol, std::uint32_t oi, std::uint32_t ts,
              std::size_t n_bid, std::size_t n_ask)
{
    const std::size_t start = f.n;
    f.u32(token);                       //  0
    f.u32(last);                        //  4
    f.u32(lastq);                       //  8
    f.u32(0);                           // 12 avg
    f.u32(vol);                         // 16
    f.u32(0);                           // 20 total buy
    f.u32(0);                           // 24 total sell
    f.u32(0); f.u32(0); f.u32(0); f.u32(0);   // 28,32,36,40 ohlc
    f.u32(0);                           // 44 last trade time
    f.u32(oi);                          // 48
    f.u32(0); f.u32(0);                 // 52,56 oi hi/lo
    f.u32(ts);                          // 60 exchange timestamp, SECONDS
    // 64: five buy levels
    for (std::size_t k = 0; k < 5; ++k) {
        if (k < n_bid) {
            f.u32(static_cast<std::uint32_t>(100 + k));            // qty
            f.u32(static_cast<std::uint32_t>(2'500'000 - k * 5));  // price, paise
            f.u16(static_cast<std::uint16_t>(3 + k));              // orders
            f.u16(0);                                              // pad
        } else {
            f.u32(0); f.u32(0); f.u16(0); f.u16(0);
        }
    }
    // 124: five sell levels
    for (std::size_t k = 0; k < 5; ++k) {
        if (k < n_ask) {
            f.u32(static_cast<std::uint32_t>(200 + k));
            f.u32(static_cast<std::uint32_t>(2'500'500 + k * 5));
            f.u16(static_cast<std::uint16_t>(4 + k));
            f.u16(0);
        } else {
            f.u32(0); f.u32(0); f.u16(0); f.u16(0);
        }
    }
    if (f.n - start != 184) {
        std::printf("  FAIL: full packet is %zu bytes, expected 184\n", f.n - start);
        ++failures;
    }
}

void put_ltp(Buf& f, std::uint32_t token, std::uint32_t last)
{
    f.u32(token);
    f.u32(last);
}

/// Wrap packets into a frame: [u16 count] then [u16 len][payload] each.
Buf frame(const Buf* pkts, const std::size_t* lens, std::size_t n)
{
    Buf out;
    out.u16(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        out.u16(static_cast<std::uint16_t>(lens[i]));
        if (out.n + lens[i] > Buf::kCap) {
            std::printf("  FAIL: frame would overflow the test buffer\n");
            ++failures;
            return out;
        }
        std::memcpy(out.b + out.n, pkts[i].b, lens[i]);
        out.n += lens[i];
    }
    return out;
}

ContractSpec spec_for(std::uint32_t token, const char* sym, Segment seg)
{
    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.lot_size = LotSize{75};
    s.tick_size = Price{5};
    s.price_scale = 100;
    s.expiry = seg == Segment::Cash ? Timestamp::epoch()
                                    : Timestamp{1790226000000000000LL};
    s.valid_from = Timestamp{1787000000000000000LL};
    s.valid_to = Timestamp::max();
    s.exchange = Exchange::NSE;
    s.segment = seg;
    s.opt_type = OptionType::None;
    s.source = SpecSource::KiteDump;
    s.token[static_cast<std::size_t>(FeedSource::Kite)] = token;
    std::snprintf(s.symbol, sizeof(s.symbol), "%s", sym);
    std::snprintf(s.underlying, sizeof(s.underlying), "NIFTY");
    return s;
}

Tick g_ticks[64];
DepthUpdate g_depths[64];
std::uint32_t g_seq = 0;

auto run(const Buf& f, std::size_t len)
{
    g_seq = 0;
    return decode_kite_frame(f.b, len, g_store, kRecv, g_seq,
                             g_ticks, 64, g_depths, 64);
}

void setup_store()
{
    (void)g_store.add(spec_for(kFoToken, "NIFTY26SEPFUT", Segment::Fut));
    (void)g_store.add(spec_for(kCdToken, "USDINR26SEPFUT", Segment::Currency));
    (void)g_store.add(spec_for(kIndexToken, "NIFTY", Segment::Cash));
    const auto b = g_store.add(spec_for(kBlockedTok, "BLOCKEDFUT", Segment::Fut));
    if (b) {
        (void)g_store.block(*b);
    }
}

// ── 1 ────────────────────────────────────────────────────────────────────
void frame_splitting()
{
    std::printf("\n1 frame_splitting\n");

    Buf p[3];
    put_ltp(p[0], kFoToken, 2'500'000);
    put_full(p[1], kFoToken, 2'500'100, 75, 12345, 999, 1787888700u, 5, 5);
    put_ltp(p[2], kFoToken, 2'500'200);
    const std::size_t lens[3] = {8, 184, 8};
    Buf f = frame(p, lens, 3);

    const auto r = run(f, f.n);
    check(r.has_value(), "a three-packet frame decodes");
    check(r.has_value() && r->ticks == 3, "three ticks");
    check(r.has_value() && r->depths == 1, "and one depth -- only full mode carries it");

    // A 1-byte heartbeat is normal operation, not a malformed frame.
    Buf hb;
    hb.u8(0);
    const auto h = run(hb, 1);
    check(h.has_value() && h->ticks == 0,
          "a 1-byte heartbeat gives zero packets and NO error");
    const auto z = run(hb, 0);
    check(z.has_value() && z->ticks == 0, "an empty buffer likewise");

    // A length that overruns loses the framing entirely.
    Buf bad;
    bad.u16(1);
    bad.u16(500);          // claims 500 bytes
    bad.u32(kFoToken);
    const auto e = run(bad, bad.n);
    check(!e.has_value() && e.error() == KiteDecodeError::ShortFrame,
          "a packet length past the buffer is ShortFrame -- framing is lost");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void ltp_mode()
{
    std::printf("\n2 ltp_mode\n");

    Buf p[1];
    put_ltp(p[0], kFoToken, 2'500'050);
    const std::size_t lens[1] = {8};
    Buf f = frame(p, lens, 1);

    const auto r = run(f, f.n);
    check(r.has_value() && r->ticks == 1, "one tick");
    check(r.has_value() && r->depths == 0, "LTP mode emits no depth");
    check(g_ticks[0].last.raw() == 2'500'050,
          "the price is the wire integer -- already paise at scale 100");
    check(has_flag(g_ticks[0].flags, TickFlag::NoExchangeTs),
          "and NoExchangeTs is set: this mode carries no timestamp at all");
    check(g_ticks[0].exchange_ts == kRecv,
          "so exchange_ts falls back to recv_ts, keeping ordering usable");
    check(g_ticks[0].source == FeedSource::Kite, "source records the feed");
    check(g_ticks[0].last_qty.raw() == 0 && g_ticks[0].volume.raw() == 0,
          "and no volume is invented for a mode that does not carry it");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void full_mode_prices_and_oi()
{
    std::printf("\n3 full_mode_prices_and_oi\n");

    const std::uint32_t exch_secs = 1787888700u;
    Buf p[1];
    put_full(p[0], kFoToken, 2'500'100, 75, 987654, 4242, exch_secs, 5, 5);
    const std::size_t lens[1] = {184};
    Buf f = frame(p, lens, 1);

    const auto r = run(f, f.n);
    check(r.has_value() && r->ticks == 1 && r->depths == 1,
          "full mode emits a Tick AND a DepthUpdate");
    check(g_ticks[0].last.raw() == 2'500'100, "last price");
    check(g_ticks[0].last_qty.raw() == 75, "last quantity");
    check(g_ticks[0].volume.raw() == 987654, "cumulative volume");
    check(g_ticks[0].oi == 4242, "open interest");

    // D5: Kite's exchange timestamp is SECONDS. Sub-second exchange latency is
    // not measurable from this feed at all.
    check(g_ticks[0].exchange_ts.ns_since_epoch()
              == static_cast<std::int64_t>(exch_secs) * 1'000'000'000LL,
          "exchange_ts converts from Unix SECONDS");
    check(g_ticks[0].exchange_ts.ns_since_epoch() % 1'000'000'000LL == 0,
          "and its sub-second digits are all zero -- one-second resolution, so "
          "exchange latency is not measurable from Kite");
    check(!has_flag(g_ticks[0].flags, TickFlag::NoExchangeTs),
          "NoExchangeTs is clear -- this mode does carry one");
    check(g_ticks[0].recv_ts == kRecv,
          "while recv_ts keeps full nanosecond resolution");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void depth_levels_and_padding()
{
    std::printf("\n4 depth_levels_and_padding\n");

    Buf p[1];
    put_full(p[0], kFoToken, 2'500'100, 75, 1, 1, 1787888700u, 5, 5);
    std::size_t lens[1] = {184};
    Buf f = frame(p, lens, 1);
    auto r = run(f, f.n);
    check(r.has_value() && g_depths[0].bid_levels == 5, "five populated bids");
    check(g_depths[0].ask_levels == 5, "five populated asks");
    check(g_depths[0].bid[0].px.raw() == 2'500'000, "best bid price");
    check(g_depths[0].bid[0].qty.raw() == 100, "best bid quantity");
    check(g_depths[0].bid[0].orders == 3, "best bid order count");
    check(g_depths[0].ask[0].px.raw() == 2'500'500, "best ask price");
    check(mid(g_depths[0]).has_value() && mid(g_depths[0])->raw() == 2'500'250,
          "and the book mids correctly");

    // Two real levels, three zero-padded.
    Buf q[1];
    put_full(q[0], kFoToken, 2'500'100, 75, 1, 1, 1787888700u, 2, 0);
    Buf g = frame(q, lens, 1);
    r = run(g, g.n);
    check(r.has_value() && g_depths[0].bid_levels == 2,
          "two real levels are counted as two, not five");
    check(g_depths[0].ask_levels == 0, "a fully zeroed side counts zero");
    check(g_depths[0].bid[2].px.raw() == 0,
          "the padded slot's price is 0 -- counting it would be a bid at "
          "price zero, which is infinite edge");
    check(best_ask(g_depths[0]) == nullptr, "so best_ask returns nullptr");
    check(!mid(g_depths[0]).has_value(), "and there is no mid");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// D4, and the P0-01 carried debt arriving for real.
void currency_derivative_is_refused()
{
    std::printf("\n5 currency_derivative_is_refused\n");

    check(kite_price_scale(kite_segment_of(kCdToken)) == 10'000'000,
          "an NseCD token's wire scale is 10^7 units per rupee");
    check(kite_price_scale(kite_segment_of(kFoToken)) == 100,
          "while NseFO is 100 -- the wire value IS paise");

    // What truncation would have cost: the same integer read at the wrong
    // scale is off by 10^5.
    const std::int64_t wire = 8'512'345'678LL;          // 851.2345678 rupees
    check(wire / 10'000'000LL == 851, "at 10^7 that is Rs 851");
    check(wire / 100LL == 85'123'456LL,
          "read as paise it is Rs 851'234.56 -- wrong by a factor of 100'000");

    // A CD packet between two good ones. It must be skipped and counted, NOT
    // fatal: aborting would mean one CD instrument in the subscription kills
    // every frame it appears in, and the feed goes dark looking like a decoder
    // bug when the real fault is one line of universe config.
    Buf p[3];
    put_ltp(p[0], kFoToken, 2'500'100);
    put_full(p[1], kCdToken, 2'500'100, 75, 1, 1, 1787888700u, 5, 5);
    put_ltp(p[2], kFoToken, 2'500'200);
    const std::size_t lens[3] = {8, 184, 8};
    Buf f = frame(p, lens, 3);

    const auto r = run(f, f.n);
    check(r.has_value(), "the frame still decodes");
    check(r.has_value() && r->unsupported_scale == 1,
          "the currency derivative is counted, never decoded");
    check(r.has_value() && r->ticks == 2,
          "and BOTH surrounding packets survive -- one bad instrument does "
          "not take the feed down");
    check(g_ticks[1].last.raw() == 2'500'200,
          "including the one after it");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void unknown_and_blocked_tokens()
{
    std::printf("\n6 unknown_and_blocked_tokens\n");

    Buf p[3];
    put_ltp(p[0], kAbsentTok, 111);      // not in the store
    put_ltp(p[1], kBlockedTok, 222);     // in the store, blocked
    put_ltp(p[2], kFoToken, 2'500'333);  // good
    const std::size_t lens[3] = {8, 8, 8};
    Buf f = frame(p, lens, 3);

    const auto r = run(f, f.n);
    check(r.has_value(), "the frame still decodes");
    check(r.has_value() && r->unknown_token == 1,
          "the unmappable token is counted, not silently dropped");
    check(r.has_value() && r->blocked == 1,
          "the blocked one is counted separately -- P1-06 already refused it");
    check(r.has_value() && r->ticks == 1,
          "and the good packet AFTER both still decodes: one bad packet does "
          "not discard the frame");
    check(g_ticks[0].last.raw() == 2'500'333, "with the right price");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void index_packets()
{
    std::printf("\n7 index_packets\n");

    check(kite_segment_of(kIndexToken) == KiteSegment::Indices,
          "256265 & 0xFF is 9 -- NIFTY 50 is an index");

    // 32-byte index full: last at 4, timestamp at 28.
    Buf p[1];
    p[0].u32(kIndexToken);
    p[0].u32(2'408'040);
    p[0].u32(0); p[0].u32(0); p[0].u32(0); p[0].u32(0); p[0].u32(0);
    p[0].u32(1787888700u);
    std::size_t lens[1] = {32};
    Buf f = frame(p, lens, 1);
    auto r = run(f, f.n);
    check(r.has_value() && r->ticks == 1, "an index full packet decodes");
    check(r.has_value() && r->depths == 0,
          "and emits NO depth -- an index is not traded");
    check(g_ticks[0].last.raw() == 2'408'040, "the index level");
    check(!has_flag(g_ticks[0].flags, TickFlag::NoExchangeTs),
          "the 32-byte form carries a timestamp");
    check(g_ticks[0].volume.raw() == 0 && g_ticks[0].oi == 0,
          "with no volume or OI invented for it");

    // 28-byte index quote: no timestamp.
    Buf q[1];
    q[0].u32(kIndexToken);
    q[0].u32(2'408'050);
    q[0].u32(0); q[0].u32(0); q[0].u32(0); q[0].u32(0); q[0].u32(0);
    lens[0] = 28;
    Buf g = frame(q, lens, 1);
    r = run(g, g.n);
    check(r.has_value() && r->ticks == 1, "an index quote packet decodes");
    check(has_flag(g_ticks[0].flags, TickFlag::NoExchangeTs),
          "and the 28-byte form sets NoExchangeTs");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void big_endian_and_alignment()
{
    std::printf("\n8 big_endian_and_alignment\n");

    // A pattern whose big-endian and little-endian readings differ obviously.
    const std::uint8_t pat[4] = {0x01, 0x02, 0x03, 0x04};
    check(detail::be32(pat) == 0x01020304u,
          "be32 reads big-endian 0x01020304");
    check(detail::be32(pat) != 0x04030201u,
          "and NOT the little-endian 0x04030201 that a raw cast would give "
          "on x86");
    check(detail::be16(pat) == 0x0102u, "be16 likewise");

    // The same frame decoded from an ODD offset. A reinterpret_cast onto the
    // payload would be a misaligned load here -- UB, not merely slow.
    Buf p[1];
    put_full(p[0], kFoToken, 2'500'100, 75, 987654, 4242, 1787888700u, 5, 5);
    const std::size_t lens[1] = {184};
    Buf f = frame(p, lens, 1);

    const auto a = run(f, f.n);
    check(a.has_value(), "aligned decode succeeds");
    const Tick aligned = g_ticks[0];
    const DepthUpdate aligned_d = g_depths[0];

    static std::uint8_t shifted[1024];
    std::memset(shifted, 0, sizeof(shifted));
    std::memcpy(shifted + 1, f.b, f.n);          // offset by ONE byte
    g_seq = 0;
    const auto b = decode_kite_frame(shifted + 1, f.n, g_store, kRecv, g_seq,
                                     g_ticks, 64, g_depths, 64);
    check(b.has_value(), "odd-aligned decode also succeeds");
    check(std::memcmp(&aligned, &g_ticks[0], sizeof(Tick)) == 0,
          "and produces a BYTE-IDENTICAL Tick -- no misaligned word load");
    check(std::memcmp(&aligned_d, &g_depths[0], sizeof(DepthUpdate)) == 0,
          "and an identical DepthUpdate");
}

// ── output capacity ──────────────────────────────────────────────────────
void output_capacity()
{
    std::printf("\noutput capacity\n");

    Buf p[3];
    put_ltp(p[0], kFoToken, 1);
    put_ltp(p[1], kFoToken, 2);
    put_ltp(p[2], kFoToken, 3);
    const std::size_t lens[3] = {8, 8, 8};
    Buf f = frame(p, lens, 3);

    g_seq = 0;
    const auto r = decode_kite_frame(f.b, f.n, g_store, kRecv, g_seq,
                                     g_ticks, 2, g_depths, 64);
    check(!r.has_value() && r.error() == KiteDecodeError::OutputFull,
          "a frame larger than the caller's array is OutputFull, not a "
          "silent truncation");

    // The check is up front, so it is ALL-or-nothing. A mid-stream abort would
    // have partially filled the array and then returned an error carrying no
    // count, losing every tick already decoded with no way to know how many.
    g_ticks[0] = Tick{};
    g_seq = 0;
    const auto r2 = decode_kite_frame(f.b, f.n, g_store, kRecv, g_seq,
                                      g_ticks, 2, g_depths, 64);
    check(!r2.has_value(), "still refused");
    check(g_ticks[0].seq == 0 && g_ticks[0].id == InstrumentId{},
          "and NOTHING was written -- the capacity check runs before the "
          "first packet, not after two of them");
    check(g_seq == 0, "the caller's sequence counter is untouched too");

    // Exactly enough room works.
    g_seq = 0;
    const auto ok = decode_kite_frame(f.b, f.n, g_store, kRecv, g_seq,
                                      g_ticks, 3, g_depths, 3);
    check(ok.has_value() && ok->ticks == 3, "exactly enough capacity decodes");
}

// ── gate 6: the latency budget ───────────────────────────────────────────
void benchmark()
{
    std::printf("\ndecode latency -- ALTAIR_HOT, ROADMAP 11 budget 3 us p99\n");

    // A realistic frame: ten full-mode packets with five levels a side.
    Buf p[10];
    std::size_t lens[10];
    for (std::size_t i = 0; i < 10; ++i) {
        put_full(p[i], kFoToken, 2'500'100 + static_cast<std::uint32_t>(i),
                 75, 987654, 4242, 1787888700u, 5, 5);
        lens[i] = 184;
    }
    Buf f = frame(p, lens, 10);

    static Tick tbuf[64];
    static DepthUpdate dbuf[64];
    std::uint32_t seq = 0;

    // Batch-timed: a single decode is far below clock resolution, so time a
    // million of them between one pair of reads and divide. Bracketing each
    // call would measure the harness, which is the P0-03 lesson.
    constexpr int kIters = 200'000;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    for (int i = 0; i < kIters; ++i) {
        const auto r = decode_kite_frame(f.b, f.n, g_store, kRecv, seq,
                                         tbuf, 64, dbuf, 64);
        sink += r.has_value() ? r->ticks : 0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double per_frame_ns = secs * 1e9 / kIters;
    const double per_packet_ns = per_frame_ns / 10.0;

    std::printf("  %d frames x 10 full packets in %.3f s\n", kIters, secs);
    std::printf("  %.0f ns per frame, %.0f ns per packet\n",
                per_frame_ns, per_packet_ns);
    check(sink == static_cast<std::size_t>(kIters) * 10,
          "every iteration decoded ten ticks -- the loop was not optimised away");
    check(per_frame_ns < 3000.0,
          "a full 10-packet frame decodes well inside the 3 us budget");
}

} // namespace

int main()
{
    std::printf("altair feed kite_decoder tests\n");
    setup_store();
    frame_splitting();
    ltp_mode();
    full_mode_prices_and_oi();
    depth_levels_and_padding();
    currency_derivative_is_refused();
    unknown_and_blocked_tokens();
    index_packets();
    big_endian_and_alignment();
    output_capacity();
    benchmark();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
