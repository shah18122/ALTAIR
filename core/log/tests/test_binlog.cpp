// P0-07 acceptance tests for core/log/binlog.hpp and core/log/decoder.hpp.
// Plain main() (Catch2 still unavailable).

#include <log/binlog.hpp>
#include <log/decoder.hpp>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

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

void check_str(const char* got, const char* want, const char* what)
{
    if (std::strcmp(got, want) == 0) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n         got  \"%s\"\n         want \"%s\"\n",
                    what, got, want);
    }
}

/// The clock under test: real TSC where the CPU allows, otherwise an explicit
/// fallback — exactly as the P0-03 tests do.
const altair::TscClock& clock_under_test()
{
    static const altair::TscClock c = [] {
        auto r = altair::TscClock::create();
        return r.has_value() ? *r : altair::TscClock::create_fallback();
    }();
    return c;
}

bool ends_with(const char* s, const char* suffix)
{
    const std::size_t ls = std::strlen(s);
    const std::size_t lf = std::strlen(suffix);
    return ls >= lf && std::strcmp(s + (ls - lf), suffix) == 0;
}

} // namespace

using namespace altair;

void test_log_record_layout()
{
    check(sizeof(LogRecord) == 64, "sizeof(LogRecord) == 64, exactly one cache line");
    check(alignof(LogRecord) >= 8, "alignof(LogRecord) >= 8");
    check(std::is_trivially_copyable_v<LogRecord>, "LogRecord is trivially copyable");
    check(kMaxLogArgs == 5, "kMaxLogArgs == 5");

    // The ring must accept it — this is the reason for the size discipline.
    check(SpscRing<LogRecord, 8>::capacity() == 8, "SpscRing<LogRecord,8> instantiates");

    std::printf("        sizeof(LogRecord) = %zu, alignof = %zu\n",
                sizeof(LogRecord), alignof(LogRecord));
}

void test_log_registry()
{
    LogRegistry reg;
    check(reg.count() == 0, "a fresh registry is empty");

    static const LogSite s1{"a {}", "t.cpp", 10, LogLevel::Info};
    const auto id1 = reg.register_site(s1);
    check(id1 == 0, "first site gets id 0");
    check(reg.count() == 1, "count == 1");
    check(reg.site(id1)->line == 10, "site line round-trips");
    check(reg.site(id1)->fmt == s1.fmt, "the format text is referenced, not copied");

    static const LogSite s2{"b", "t.cpp", 11, LogLevel::Warn};
    check(reg.register_site(s2) == 1, "second site gets id 1");
    check(reg.count() == 2, "count == 2");

    check(reg.site(2) == nullptr, "an id beyond count() is unknown");
    check(reg.site(LogRegistry::kInvalidSite) == nullptr, "kInvalidSite is unknown");
}

void test_log_level_filtering()
{
    LogRegistry reg;
    static const LogSite s{"x {}", "t.cpp", 1, LogLevel::Info};
    const auto id = reg.register_site(s);

    SpscRing<LogRecord, 16> ring;
    Logger<16> log{ring, LogLevel::Warn};

    check(!log.write(id, LogLevel::Info, 1), "below threshold returns false");
    check(log.filtered() == 1, "filtered counter moved");
    check(log.written() == 0, "written counter did not");
    check(ring.size_approx() == 0, "nothing was pushed");

    check(log.write(id, LogLevel::Warn, 1), "at threshold is accepted");
    check(log.write(id, LogLevel::Error, 2), "above threshold is accepted");
    check(log.written() == 2, "written == 2");
    check(log.filtered() == 1, "filtered still 1");

    log.set_min_level(LogLevel::Trace);
    check(log.write(id, LogLevel::Trace, 3), "lowering the threshold admits Trace");
    check(log.written() == 3, "written == 3");
}

void test_log_arg_packing()
{
    LogRegistry reg;
    static const LogSite s{"x", "t.cpp", 1, LogLevel::Trace};
    const auto id = reg.register_site(s);

    SpscRing<LogRecord, 16> ring;
    Logger<16> log{ring, LogLevel::Trace};
    LogRecord r{};

    (void)log.write(id, LogLevel::Info, std::int64_t{-42});
    (void)ring.try_pop(r);
    check(r.arg_count == 1 && r.arg_tags[0] == ArgTag::I64, "int64 tagged I64");
    check(std::bit_cast<std::int64_t>(r.args[0]) == -42, "int64 value round-trips");

    (void)log.write(id, LogLevel::Info, std::uint64_t{42});
    (void)ring.try_pop(r);
    check(r.arg_tags[0] == ArgTag::U64 && r.args[0] == 42, "uint64 tagged U64");

    (void)log.write(id, LogLevel::Info, 3.5);
    (void)ring.try_pop(r);
    check(r.arg_tags[0] == ArgTag::F64, "double tagged F64");
    check(std::bit_cast<double>(r.args[0]) == 3.5, "double value round-trips");

    // bool is an integral type — it must be tested BEFORE the integral branch.
    (void)log.write(id, LogLevel::Info, true);
    (void)ring.try_pop(r);
    check(r.arg_tags[0] == ArgTag::Bool, "bool tagged Bool, NOT I64");
    check(r.args[0] == 1, "bool payload is 1");

    (void)log.write(id, LogLevel::Info, -7);
    (void)ring.try_pop(r);
    check(r.arg_tags[0] == ArgTag::I64, "plain int tagged I64");
    check(std::bit_cast<std::int64_t>(r.args[0]) == -7, "plain int round-trips");

    (void)log.write(id, LogLevel::Info);
    (void)ring.try_pop(r);
    check(r.arg_count == 0, "no args -> arg_count 0");
    check(r.arg_tags[0] == ArgTag::None && r.args[0] == 0, "unused slots are zeroed");

    (void)log.write(id, LogLevel::Info, 1, 2, 3, 4, 5);
    (void)ring.try_pop(r);
    check(r.arg_count == 5, "exactly kMaxLogArgs fits");
    check(r.flags == 0, "and is not flagged truncated");

    (void)log.write(id, LogLevel::Info, 1, 2, 3, 4, 5, 6);
    (void)ring.try_pop(r);
    check(r.arg_count == 5, "one too many: the first five survive");
    check((r.flags & kLogFlagArgsTruncated) != 0, "and the record is flagged");
    check(std::bit_cast<std::int64_t>(r.args[4]) == 5, "the fifth arg is intact");
}

void test_log_decode_message()
{
    LogRegistry reg;
    static const LogSite s_order{"order {} qty {} px {}", "t.cpp", 10, LogLevel::Info};
    static const LogSite s_none {"no args here",          "t.cpp", 11, LogLevel::Info};
    static const LogSite s_flag {"flag {}",               "t.cpp", 12, LogLevel::Info};
    static const LogSite s_short{"a {} b {}",             "t.cpp", 13, LogLevel::Info};
    static const LogSite s_extra{"just {}",               "t.cpp", 14, LogLevel::Info};
    static const LogSite s_brace{"brace { here",          "t.cpp", 15, LogLevel::Info};
    static const LogSite s_spec {"spec {:.2f}",           "t.cpp", 16, LogLevel::Info};

    const auto id_order = reg.register_site(s_order);
    const auto id_none  = reg.register_site(s_none);
    const auto id_flag  = reg.register_site(s_flag);
    const auto id_short = reg.register_site(s_short);
    const auto id_extra = reg.register_site(s_extra);
    const auto id_brace = reg.register_site(s_brace);
    const auto id_spec  = reg.register_site(s_spec);

    SpscRing<LogRecord, 16> ring;
    Logger<16> log{ring, LogLevel::Trace};
    LogDecoder dec{reg, clock_under_test()};

    char buf[256];
    LogRecord r{};

    auto emit = [&](std::uint32_t id, auto&&... a) {
        (void)log.write(id, LogLevel::Info, a...);
        (void)ring.try_pop(r);
        (void)dec.decode_message(r, buf, sizeof(buf));
    };

    emit(id_order, std::uint64_t{7}, std::int64_t{-50}, 1.5);
    check_str(buf, "order 7 qty -50 px 1.5", "three mixed args substitute");

    emit(id_none);
    check_str(buf, "no args here", "a format with no placeholders");

    emit(id_flag, true);
    check_str(buf, "flag true", "bool true renders as text");
    emit(id_flag, false);
    check_str(buf, "flag false", "bool false renders as text");

    emit(id_short, 1);
    check_str(buf, "a 1 b {?}", "a placeholder with no argument becomes {?}");

    emit(id_extra, 1, 2, 3);
    check_str(buf, "just 1 (+2 unused)", "arguments with nowhere to go are reported");

    emit(id_brace);
    check_str(buf, "brace { here", "a lone brace is literal");

    // `{:.2f}` is not a placeholder to this decoder, so the argument goes
    // unconsumed — and reporting that is right: it surfaces the unsupported
    // spec rather than silently swallowing a value the author expected printed.
    emit(id_spec, 1.5);
    check_str(buf, "spec {:.2f} (+1 unused)",
              "a format spec is literal, and its unused argument is reported");

    // decode_line carries the site and the level, and ends with the message.
    (void)log.write(id_order, LogLevel::Info, std::uint64_t{7}, std::int64_t{-50}, 1.5);
    (void)ring.try_pop(r);
    const std::size_t n = dec.decode_line(r, buf, sizeof(buf));
    check(n > 0, "decode_line produced output");
    check(std::strstr(buf, "t.cpp:10") != nullptr, "decode_line names the site");
    check(std::strstr(buf, "INFO ") != nullptr, "decode_line carries the level");
    check(ends_with(buf, "order 7 qty -50 px 1.5"), "decode_line ends with the message");
    std::printf("        %s\n", buf);
}

void test_log_decode_truncation()
{
    LogRegistry reg;
    static const LogSite s{"a very long message indeed {}", "t.cpp", 1, LogLevel::Info};
    const auto id = reg.register_site(s);

    SpscRing<LogRecord, 8> ring;
    Logger<8> log{ring, LogLevel::Trace};
    LogDecoder dec{reg, clock_under_test()};

    (void)log.write(id, LogLevel::Info, 12345);
    LogRecord r{};
    (void)ring.try_pop(r);

    // A canary immediately after the buffer catches a one-past write.
    struct { char buf[8]; char canary; } b{};
    b.canary = '\x7F';

    const std::size_t n = dec.decode_message(r, b.buf, sizeof(b.buf));
    check(n <= 7, "truncated output fits in cap-1");
    check(b.buf[n] == '\0', "output is always NUL-terminated");
    check(b.canary == '\x7F', "nothing was written past the buffer");

    check(dec.decode_message(r, b.buf, 0) == 0, "cap 0 writes nothing");
    b.buf[0] = 'X';
    check(dec.decode_message(r, b.buf, 1) == 0, "cap 1 writes only the NUL");
    check(b.buf[0] == '\0', "and terminates it");

    LogRecord bad{};
    bad.site_id = 999;
    char big[64];
    check(dec.decode_message(bad, big, sizeof(big)) == 0,
          "an unknown site id yields 0, not a crash");
    check(dec.decode_line(bad, big, sizeof(big)) == 0,
          "decode_line likewise");
}

void test_log_ring_full_drops_and_marks_gap()
{
    LogRegistry reg;
    static const LogSite s{"v {}", "t.cpp", 1, LogLevel::Info};
    const auto id = reg.register_site(s);

    SpscRing<LogRecord, 4> ring;
    Logger<4> log{ring, LogLevel::Trace};

    for (int i = 0; i < 4; ++i) {
        (void)log.write(id, LogLevel::Info, i);
    }
    check(log.written() == 4, "the ring filled");
    check(log.dropped() == 0, "nothing dropped yet");

    check(!log.write(id, LogLevel::Info, 99), "a full ring rejects");
    check(log.dropped() == 1, "dropped counter moved");
    check(log.written() == 4, "written did not");
    check(!log.write(id, LogLevel::Info, 98), "and again");
    check(log.dropped() == 2, "dropped == 2");

    // Free a slot, then write: the gap is marked IN THE STREAM.
    LogRecord r{};
    (void)ring.try_pop(r);
    check(log.write(id, LogLevel::Info, 100), "a freed slot accepts a write");
    while (ring.try_pop(r)) { }        // drain to the newest
    check(r.dropped_before == 2, "the record after the gap carries dropped_before == 2");

    check(log.write(id, LogLevel::Info, 101), "next write succeeds");
    (void)ring.try_pop(r);
    check(r.dropped_before == 0, "and the gap counter reset after a success");

    // The saturation is documented behaviour, so prove it saturates rather
    // than wrapping — a wrap would report a tiny gap after a huge one.
    SpscRing<LogRecord, 2> tiny;
    Logger<2> sat{tiny, LogLevel::Trace};
    (void)sat.write(id, LogLevel::Info, 1);
    (void)sat.write(id, LogLevel::Info, 2);
    for (int i = 0; i < 70000; ++i) {
        (void)sat.write(id, LogLevel::Info, i);   // all rejected
    }
    check(sat.dropped() == 70000, "70000 drops counted");
    (void)tiny.try_pop(r);
    check(sat.write(id, LogLevel::Info, 7), "one slot freed");
    while (tiny.try_pop(r)) { }
    check(r.dropped_before == 65535, "dropped_before saturates at 65535, never wraps");
}

void test_log_two_thread_roundtrip()
{
    constexpr int kN = 200'000;

    static LogRegistry reg;
    static const LogSite s{"seq {}", "t.cpp", 1, LogLevel::Info};
    static const auto id = reg.register_site(s);

    static SpscRing<LogRecord, 1024> ring;
    static Logger<1024> log{ring, LogLevel::Trace};
    LogDecoder dec{reg, clock_under_test()};

    const auto t0 = std::chrono::steady_clock::now();

    std::thread producer([] {
        for (int i = 0; i < kN; ++i) {
            while (!log.write(id, LogLevel::Info, static_cast<std::uint64_t>(i))) {
                std::this_thread::yield();
            }
        }
    });

    bool order_ok = true;
    bool decode_ok = true;
    char buf[128];
    LogRecord r{};
    for (int i = 0; i < kN; ++i) {
        while (!ring.try_pop(r)) {
            std::this_thread::yield();
        }
        if (r.args[0] != static_cast<std::uint64_t>(i)) { order_ok = false; }
        if (dec.decode_message(r, buf, sizeof(buf)) == 0) { decode_ok = false; }
    }

    producer.join();
    const auto t1 = std::chrono::steady_clock::now();

    check(order_ok, "every argument arrived exactly once, in order");
    check(decode_ok, "every record decoded");
    check(log.written() == static_cast<std::uint64_t>(kN), "written == 200'000");
    check(ring.size_approx() == 0, "ring drained");

    // dropped() is deliberately NOT asserted zero. This producer spins on a
    // full ring, and every failed attempt increments the counter even though
    // the record is then delivered by the retry — dropped() counts ATTEMPTS,
    // not losses. Delivery is proved by order_ok and written() above.
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("        %d records logged + decoded in %.3f s = %.2f M rec/s\n",
                kN, sec, static_cast<double>(kN) / sec / 1e6);
    std::printf("        %llu retry attempts hit a full ring (all delivered)\n",
                static_cast<unsigned long long>(log.dropped()));
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");

    static LogRegistry reg;
    static const LogSite s{"bench {} {} {}", "t.cpp", 1, LogLevel::Info};
    const auto id = reg.register_site(s);

    static SpscRing<LogRecord, 1024> ring;
    Logger<1024> log{ring, LogLevel::Trace};
    LogDecoder dec{reg, clock_under_test()};

    constexpr int kOps = 500'000;
    LogRecord r{};
    std::uint64_t sink = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        (void)log.write(id, LogLevel::Info,
                        static_cast<std::uint64_t>(i), std::int64_t{-1}, 2.5);
        // Drain immediately so the ring never fills; the pop is part of the
        // measurement and is named as such.
        if (ring.try_pop(r)) { sink += r.args[0]; }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                    / static_cast<double>(kOps);
    std::printf("  Logger::write(u64, i64, f64)  %6.2f ns/call (budget < 40)  %s\n",
                ns, ns < 40.0 ? "OK" : "OVER");
    std::printf("    (includes the paired try_pop that keeps the ring drained)\n");

    // Decode is off the hot path and has no budget, but a decoder slower than
    // the producer is a ring that fills.
    (void)log.write(id, LogLevel::Info, std::uint64_t{7}, std::int64_t{-1}, 2.5);
    (void)ring.try_pop(r);
    char buf[160];
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        sink += dec.decode_line(r, buf, sizeof(buf));
    }
    const auto t3 = std::chrono::steady_clock::now();
    const double ns2 = std::chrono::duration<double, std::nano>(t3 - t2).count()
                     / static_cast<double>(kOps);
    std::printf("  LogDecoder::decode_line       %6.2f ns/call (no budget — off path)\n", ns2);
    std::printf("    ratio decode:write = %.1fx\n", ns2 / ns);

    if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf("  (unreachable)\n"); }
}

} // namespace

int main()
{
    std::printf("altair core/log binlog + decoder tests\n");
    test_log_record_layout();
    test_log_registry();
    test_log_level_filtering();
    test_log_arg_packing();
    test_log_decode_message();
    test_log_decode_truncation();
    test_log_ring_full_drops_and_marks_gap();
    test_log_two_thread_roundtrip();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
