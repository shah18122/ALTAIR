// P2-06 / P2-07 acceptance tests for feed/tick_store.hpp.
//
// The central one is round_trips_byte_for_byte: rule 6 says the replay feed
// and the live feed emit the SAME struct into the SAME pipeline, and that is
// only true if what comes back off disk is indistinguishable from what the
// decoder produced. If they ever diverge, the backtest is a lie.
//
// No check description here may contain the substring FAIL.

#include <feed/tick_store.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

constexpr Timestamp kSession{1787888700000000000LL};
const char* kPath = "altair_tick_store_test.bin";

Tick mk_tick(std::uint32_t seq)
{
    Tick t{};
    t.id = static_cast<InstrumentId>(seq % 97);
    t.seq = seq;
    t.exchange_ts = Timestamp{kSession.ns_since_epoch()
                              + static_cast<std::int64_t>(seq) * 1'000'000LL};
    t.recv_ts = Timestamp{t.exchange_ts.ns_since_epoch() + 123456};
    t.last = Price{2'500'000 + static_cast<std::int64_t>(seq)};
    t.last_qty = Qty{75};
    t.volume = Qty{1000 + static_cast<std::int64_t>(seq)};
    t.oi = 4242 + static_cast<std::int64_t>(seq);
    t.source = (seq % 2 == 0) ? FeedSource::Kite : FeedSource::Xts;
    t.flags = set_flag(0, TickFlag::Snapshot);
    return t;
}

DepthUpdate mk_depth(std::uint32_t seq)
{
    DepthUpdate d{};
    d.id = static_cast<InstrumentId>(seq % 97);
    d.seq = seq;
    d.exchange_ts = Timestamp{kSession.ns_since_epoch()
                              + static_cast<std::int64_t>(seq) * 1'000'000LL};
    d.recv_ts = Timestamp{d.exchange_ts.ns_since_epoch() + 4242};
    d.source = FeedSource::Kite;
    for (std::size_t k = 0; k < kDepthLevels; ++k) {
        d.bid[k] = DepthLevel{Price{2'500'000 - static_cast<std::int64_t>(k) * 5},
                              Qty{100 + static_cast<std::int64_t>(k)},
                              static_cast<std::uint32_t>(3 + k), 0};
        d.ask[k] = DepthLevel{Price{2'500'050 + static_cast<std::int64_t>(k) * 5},
                              Qty{200 + static_cast<std::int64_t>(k)},
                              static_cast<std::uint32_t>(4 + k), 0};
    }
    d.bid_levels = kDepthLevels;
    d.ask_levels = kDepthLevels;
    return d;
}

TickStoreWriter g_w;
TickStoreReader g_r;

// ── 1 ────────────────────────────────────────────────────────────────────
// Rule 6, made checkable.
void round_trips_byte_for_byte()
{
    std::printf("\n1 round_trips_byte_for_byte\n");

    constexpr std::uint32_t kN = 500;
    Tick sent_t[kN];
    DepthUpdate sent_d[kN];

    check(g_w.open(kPath, kSession).has_value(), "the store opens for writing");
    for (std::uint32_t i = 0; i < kN; ++i) {
        sent_t[i] = mk_tick(i);
        sent_d[i] = mk_depth(i);
        (void)g_w.write(sent_t[i]);
        (void)g_w.write(sent_d[i]);
    }
    check(g_w.stats().ticks == kN && g_w.stats().depths == kN,
          "500 ticks and 500 depth updates written");
    check(g_w.close().has_value(), "and it closes cleanly");

    const auto h = g_r.open(kPath);
    check(h.has_value(), "the reader opens it");
    check(h.has_value() && h->wire_version == kTickWireVersion,
          "the header records the WIRE version the structs were written with");
    check(h.has_value() && h->session_date_ns == kSession.ns_since_epoch(),
          "and the session date");

    std::uint32_t nt = 0, nd = 0, diffs = 0;
    TickStoreReader::Record rec{};
    for (;;) {
        const auto more = g_r.next(rec);
        if (!more.has_value() || !*more) {
            break;
        }
        if (rec.tag == RecordTag::Tick) {
            if (std::memcmp(&rec.tick, &sent_t[nt], sizeof(Tick)) != 0) {
                ++diffs;
            }
            ++nt;
        } else {
            if (std::memcmp(&rec.depth, &sent_d[nd], sizeof(DepthUpdate)) != 0) {
                ++diffs;
            }
            ++nd;
        }
    }
    (void)g_r.close();

    check(nt == kN && nd == kN, "every record read back, in order");
    check(diffs == 0,
          "and every one is BYTE-IDENTICAL to what was written -- padding "
          "included. Rule 6 holds: replay hands the pipeline exactly what the "
          "live decoder did");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void interleaving_is_preserved()
{
    std::printf("\n2 interleaving_is_preserved\n");

    (void)g_w.open(kPath, kSession);
    // A deliberately irregular pattern -- ticks and depth do not alternate on
    // a real feed, and the order between them carries information.
    const char* pattern = "TTDTDDTTTD";
    for (std::uint32_t i = 0; i < 10; ++i) {
        if (pattern[i] == 'T') {
            (void)g_w.write(mk_tick(i));
        } else {
            (void)g_w.write(mk_depth(i));
        }
    }
    (void)g_w.close();

    (void)g_r.open(kPath);
    char got[11] = {};
    std::size_t n = 0;
    TickStoreReader::Record rec{};
    for (;;) {
        const auto more = g_r.next(rec);
        if (!more.has_value() || !*more) {
            break;
        }
        got[n++] = (rec.tag == RecordTag::Tick) ? 'T' : 'D';
    }
    (void)g_r.close();
    check(n == 10 && std::strcmp(got, pattern) == 0,
          "the tick/depth INTERLEAVING survives the round trip exactly -- the "
          "order between the two streams is information, not an artefact");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// D3. The refusal that matters most.
void a_shape_mismatch_is_refused()
{
    std::printf("\n3 a_shape_mismatch_is_refused\n");

    (void)g_w.open(kPath, kSession);
    (void)g_w.write(mk_tick(1));
    (void)g_w.close();

    // Corrupt the recorded struct size, as a future build with a wider Tick
    // would legitimately produce.
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    (void)::fopen_s(&f, kPath, "r+b");
#else
    f = std::fopen(kPath, "r+b");
#endif
    check(f != nullptr, "the file reopens for patching");
    if (f != nullptr) {
        std::fseek(f, static_cast<long>(offsetof(TickStoreHeader, tick_bytes)),
                   SEEK_SET);
        const std::uint32_t wrong = sizeof(Tick) + 8;
        (void)std::fwrite(&wrong, sizeof(wrong), 1, f);
        std::fclose(f);
    }

    const auto r = g_r.open(kPath);
    check(!r.has_value() && r.error() == StoreError::BadWireVersion,
          "a file whose Tick was a DIFFERENT SIZE is refused before a single "
          "record is read -- reinterpreting those bytes would produce "
          "plausible garbage, which is the worst outcome available");
    check(!g_r.is_open(), "and the reader did not stay open on a bad file");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_truncated_tail_is_reported()
{
    std::printf("\n4 a_truncated_tail_is_reported\n");

    (void)g_w.open(kPath, kSession);
    for (std::uint32_t i = 0; i < 5; ++i) {
        (void)g_w.write(mk_tick(i));
    }
    (void)g_w.close();

    // Chop the last record in half -- exactly what a crash mid-write leaves.
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    (void)::fopen_s(&f, kPath, "rb");
#else
    f = std::fopen(kPath, "rb");
#endif
    std::fseek(f, 0, SEEK_END);
    const long full = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    static char buf[65536];
    const std::size_t got = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);

#if defined(_MSC_VER)
    (void)::fopen_s(&f, kPath, "wb");
#else
    f = std::fopen(kPath, "wb");
#endif
    (void)std::fwrite(buf, 1, got - 20, f);      // 20 bytes short
    std::fclose(f);
    check(full > 20, "the file was long enough to truncate meaningfully");

    check(g_r.open(kPath).has_value(), "a truncated file still OPENS");
    std::size_t ok_records = 0;
    bool saw_truncated = false;
    TickStoreReader::Record rec{};
    for (;;) {
        const auto more = g_r.next(rec);
        if (!more.has_value()) {
            saw_truncated = (more.error() == StoreError::Truncated);
            break;
        }
        if (!*more) {
            break;
        }
        ++ok_records;
    }
    (void)g_r.close();

    check(ok_records == 4,
          "the four complete records before the cut are readable -- a crash "
          "must not cost the whole session's tape");
    check(saw_truncated,
          "and the partial fifth is reported as Truncated rather than silently "
          "treated as the end of the file");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void bad_magic_and_version()
{
    std::printf("\n5 bad_magic_and_version\n");

    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    (void)::fopen_s(&f, kPath, "wb");
#else
    f = std::fopen(kPath, "wb");
#endif
    TickStoreHeader h{};
    h.magic = 0xDEADBEEF;
    (void)std::fwrite(&h, sizeof(h), 1, f);
    std::fclose(f);

    const auto m = g_r.open(kPath);
    check(!m.has_value() && m.error() == StoreError::BadMagic,
          "a file that is not an Altair store is BadMagic, not a parse attempt");

    // Right magic, wrong file version.
#if defined(_MSC_VER)
    (void)::fopen_s(&f, kPath, "wb");
#else
    f = std::fopen(kPath, "wb");
#endif
    h.magic = kTickStoreMagic;
    h.store_version = kTickStoreVersion + 1;
    (void)std::fwrite(&h, sizeof(h), 1, f);
    std::fclose(f);

    const auto v = g_r.open(kPath);
    check(!v.has_value() && v.error() == StoreError::BadVersion,
          "and a future FILE version is BadVersion -- distinct from a struct "
          "shape mismatch, because they need different fixes");

    const auto missing = g_r.open("altair_no_such_store.bin");
    check(!missing.has_value() && missing.error() == StoreError::OpenFailed,
          "a missing file is OpenFailed, not an empty success");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void unknown_tag_stops_the_read()
{
    std::printf("\n6 unknown_tag_stops_the_read\n");

    (void)g_w.open(kPath, kSession);
    (void)g_w.write(mk_tick(1));
    (void)g_w.close();

    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    (void)::fopen_s(&f, kPath, "ab");
#else
    f = std::fopen(kPath, "ab");
#endif
    const std::uint8_t junk = 0x7F;
    (void)std::fwrite(&junk, 1, 1, f);
    std::fclose(f);

    check(g_r.open(kPath).has_value(), "the file opens");
    TickStoreReader::Record rec{};
    const auto first = g_r.next(rec);
    check(first.has_value() && *first, "the good record reads");
    const auto second = g_r.next(rec);
    check(!second.has_value() && second.error() == StoreError::BadRecord,
          "and an unknown tag is BadRecord -- the framing is lost, so "
          "everything after it is unaddressable and skipping is not an option");
    (void)g_r.close();
}

// ── 7 ────────────────────────────────────────────────────────────────────
void empty_store_reads_cleanly()
{
    std::printf("\n7 empty_store_reads_cleanly\n");

    (void)g_w.open(kPath, kSession);
    check(g_w.stats().ticks == 0, "nothing written");
    (void)g_w.close();

    const auto h = g_r.open(kPath);
    check(h.has_value(), "a header-only store opens");
    TickStoreReader::Record rec{};
    const auto more = g_r.next(rec);
    check(more.has_value() && !*more,
          "and reports a clean end of file -- zero records is a valid session, "
          "not an error");
    (void)g_r.close();
}

// ── 8 ────────────────────────────────────────────────────────────────────
void write_throughput()
{
    std::printf("\n8 write_throughput\n");

    constexpr std::uint32_t kN = 200'000;
    check(g_w.open(kPath, kSession).has_value(), "opened");
    const Tick t = mk_tick(1);
    for (std::uint32_t i = 0; i < kN; ++i) {
        (void)g_w.write(t);
    }
    const std::uint64_t bytes = g_w.stats().bytes;
    check(g_w.close().has_value(), "closed");

    check(g_w.stats().ticks == kN, "200k ticks written");
    std::printf("       %llu bytes for %u ticks = %.1f bytes each\n",
                static_cast<unsigned long long>(bytes), kN,
                static_cast<double>(bytes) / kN);
    check(bytes == sizeof(TickStoreHeader)
                   + static_cast<std::uint64_t>(kN) * (1 + sizeof(Tick)),
          "and the byte count is exactly header + N x (1 tag + 64) -- no "
          "hidden framing, so a session's tape size is predictable");

    std::remove(kPath);
}

} // namespace

int main()
{
    std::printf("altair feed tick_store tests\n");
    round_trips_byte_for_byte();
    interleaving_is_preserved();
    a_shape_mismatch_is_refused();
    a_truncated_tail_is_reported();
    bad_magic_and_version();
    unknown_tag_stops_the_read();
    empty_store_reads_cleanly();
    write_throughput();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
