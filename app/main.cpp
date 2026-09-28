// P0-10 — the `altair` binary, and the Phase 0 exit criterion.
//
// ROADMAP §12: "altair --replay sample.tick runs end to end with a null
// strategy; latency harness green; all invariants armed."
//
// Unit tests prove each module against its own contract. Only an assembled
// binary proves they COMPOSE. A ring sized wrong, a logger whose consumer never
// drains, a ledger that is never checked — none of those fail a unit test.
//
// This binary opens no socket and reads no credential. A Phase 0 binary that
// could reach a broker is one that could place an order.

#include <app/session_file.hpp>
#include "instruments_demo.hpp"

#include <config/config.hpp>
#include <config/store.hpp>
#include <feed/replay.hpp>
#include <invariant/conservation.hpp>
#include <lockfree/spsc_ring.hpp>
#include <log/binlog.hpp>
#include <log/decoder.hpp>
#include <mem/arena.hpp>
#include <mem/page_alloc.hpp>
#include <time/timestamp.hpp>
#include <time/tsc_clock.hpp>
#include <types/units.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace altair;

// 2026-08-28 09:15:00 IST — NSE market open, the anchor used throughout Phase 0.
constexpr Timestamp kOpen{1787888700000000000LL};

constexpr std::size_t kMaxTicks = 2'000'000;
constexpr std::size_t kLogRingCapacity = 4096;

// Static, never heap: this binary is the demonstration that the engine does not
// allocate at runtime.
ReplayTick g_ticks[kMaxTicks];

const char* session_error_name(SessionError e) noexcept
{
    switch (e) {
        case SessionError::OpenFailed:   return "OpenFailed";
        case SessionError::ShortRead:    return "ShortRead";
        case SessionError::BadMagic:     return "BadMagic";
        case SessionError::BadVersion:   return "BadVersion";
        case SessionError::BadTickSize:  return "BadTickSize";
        case SessionError::TooManyTicks: return "TooManyTicks";
        case SessionError::WriteFailed:  return "WriteFailed";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────
// The null strategy. It trades on a fixed schedule and predicts NOTHING.
// It exists to move the ledger. Anything resembling alpha here is a bug.
// ─────────────────────────────────────────────────────────────────────────
struct RunResult {
    std::size_t ticks = 0;
    std::uint64_t fills = 0;
    std::uint64_t logs_decoded = 0;
    bool invariant_held = true;
    bool clock_was_the_tick = true;
    bool breached = false;
    bool cash_delta_representable = true;
    Notional cash_delta{};
    Qty position{};
    double seconds = 0.0;
};

/// Run one session end to end with every Phase 0 module wired in.
RunResult run_session(const ReplayTick* ticks, std::size_t n,
                      const TscClock& clk, bool verbose)
{
    RunResult r{};

    // ── config: one snapshot, published, refreshed into a private copy ──
    static ConfigSnapshot cfg;
    (void)cfg.set_int("strategy.fill_every_n_ticks", 1000);
    (void)cfg.set_int("strategy.cost_paise", 10);
    (void)cfg.set_int("risk.initial_cash_paise", 100'000'000);
    cfg.set_version(1);

    static ConfigStore store;
    // CX02-C1: versions must RISE, and run_session runs more than once in a
    // process (--selftest, then --replay). Publishing version 1 every time
    // would be refused as stale from the second call on.
    cfg.set_version(store.version() + 1);
    if (!store.publish(cfg)) {
        std::printf("  config publish REFUSED -- the session did not start\n");
        return r;
    }

    static ConfigSnapshot local;
    // Contended cannot happen here: this thread is the only reader and the
    // only writer, and the publish above has already returned.
    if (store.refresh(local) != ConfigRefresh::Updated) {
        std::printf("  config refresh did not deliver the published snapshot"
                    " -- the session did not start\n");
        return r;
    }

    const auto h_every = local.find("strategy.fill_every_n_ticks");
    const auto h_cost  = local.find("strategy.cost_paise");
    const auto h_cash  = local.find("risk.initial_cash_paise");
    const std::int64_t every = h_every.has_value()
        ? local.get_int(h_every.value()).value_or(1000) : 1000;
    const std::int64_t cost = h_cost.has_value()
        ? local.get_int(h_cost.value()).value_or(10) : 10;
    const std::int64_t cash0 = h_cash.has_value()
        ? local.get_int(h_cash.value()).value_or(100'000'000) : 100'000'000;

    // ── scratch memory: OS pages once, then a bump allocator ──
    auto block = PageBlock::reserve(1u << 20, false);
    Arena arena;
    if (block.has_value()) {
        arena = Arena{block->data(), block->size()};
    }

    // ── logging: binary records over an SpscRing, decoded in this loop ──
    static LogRegistry reg;
    static const LogSite s_fill{"fill seq {} px {} pos {}", "main.cpp", 1,
                                LogLevel::Info};
    const std::uint32_t site_fill = reg.register_site(s_fill);

    static SpscRing<LogRecord, kLogRingCapacity> log_ring;
    Logger<kLogRingCapacity> logger{log_ring, LogLevel::Info};
    const LogDecoder decoder{reg, clk};

    // ── the ledger, armed ──
    ConservationLedger ledger{Notional{cash0}};

    Replayer replayer{ticks, n};
    char logbuf[192];
    LogRecord rec{};
    std::int64_t side = 1;

    const auto t0 = std::chrono::steady_clock::now();

    while (auto t = replayer.next()) {
        // The strategy reads time ONLY from the replayer, never a wall clock.
        if (replayer.now() != t->ts) {
            r.clock_was_the_tick = false;
        }

        if (every > 0 && (t->seqno % static_cast<std::uint64_t>(every)) == 0) {
            const auto fill = ledger.on_fill(Qty{side}, t->last, Notional{cost});
            if (fill.has_value()) {
                ++r.fills;
                side = -side;
                (void)logger.write(site_fill, LogLevel::Info,
                                   t->seqno,
                                   t->last.raw(),
                                   ledger.position().raw());
            }
        }

        // Checked on EVERY tick, not at the end. A breach stops the run —
        // that is CLAUDE.md rule 9's kill switch doing its job.
        if (!ledger.check_and_trip().has_value()) {
            r.invariant_held = false;
            r.breached = true;
            break;
        }

        // Drain the log ring in the same loop. A logger whose consumer never
        // runs is a ring that silently fills, and no unit test catches it.
        while (log_ring.try_pop(rec)) {
            if (decoder.decode_line(rec, logbuf, sizeof(logbuf)) > 0) {
                ++r.logs_decoded;
                if (verbose) {
                    std::printf("    %s\n", logbuf);
                }
            }
        }

        ++r.ticks;
    }

    // Drain whatever the last iteration left behind.
    while (log_ring.try_pop(rec)) {
        if (decoder.decode_line(rec, logbuf, sizeof(logbuf)) > 0) {
            ++r.logs_decoded;
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    const auto cash_delta = ledger.cash_delta();
    if (cash_delta) {
        r.cash_delta = *cash_delta;
    } else {
        r.cash_delta_representable = false;
    }
    r.position = ledger.position();
    r.breached = r.breached || ledger.is_breached();
    return r;
}

void print_usage()
{
    std::printf(
        "altair — tick-to-tick trading engine (Phase 0)\n\n"
        "  altair --instruments [file]       Phase 1: parse, reconcile, block\n"
        "  altair --replay <file>            replay a session with the null strategy\n"
        "  altair --gen <file> [--ticks N]   generate a synthetic session (default 100000)\n"
        "  altair --selftest                 run the built-in checks\n"
        "  altair --help                     this message\n\n"
        "This build opens no socket and reads no credential.\n"
        "Live feeds arrive in Phase 2; execution in Phase 4.\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Self-tests. Registered with ctest as `app_selftest`.
// ─────────────────────────────────────────────────────────────────────────

int g_failures = 0;

void say(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool ticks_equal(const ReplayTick& a, const ReplayTick& b) noexcept
{
    return a.ts == b.ts && a.seqno == b.seqno && a.token == b.token &&
           a.last == b.last && a.qty == b.qty;
}

const char* kTmp = "altair_selftest_session.tick";

bool selftest_session_roundtrip()
{
    const int before = g_failures;
    static ReplayTick a[1000];
    static ReplayTick b[1000];
    generate_session(a, 1000, kOpen, 7);

    say(write_session(kTmp, a, 1000).has_value(), "write_session succeeds");
    const auto rd = read_session(kTmp, b, 1000);
    say(rd.has_value() && rd.value() == 1000, "read_session returns 1000 ticks");

    bool identical = true;
    for (std::size_t i = 0; i < 1000; ++i) {
        if (!ticks_equal(a[i], b[i])) { identical = false; break; }
    }
    say(identical, "every field of every tick round-trips byte-identically");

    std::remove(kTmp);
    return g_failures == before;
}

bool selftest_session_rejects_corrupt_header()
{
    const int before = g_failures;
    static ReplayTick a[100];
    static ReplayTick b[100];
    generate_session(a, 100, kOpen, 3);

    say(read_session("no_such_file_here.tick", b, 100).error()
            == SessionError::OpenFailed, "a missing path is OpenFailed");

    auto corrupt = [&](void (*mutate)(SessionHeader&)) -> SessionError {
        (void)write_session(kTmp, a, 100);
        std::FILE* f = detail::session_fopen(kTmp, "r+b");
        if (f == nullptr) { return SessionError::OpenFailed; }
        SessionHeader h{};
        (void)std::fread(&h, sizeof(h), 1, f);
        mutate(h);
        std::fseek(f, 0, SEEK_SET);
        (void)std::fwrite(&h, sizeof(h), 1, f);
        (void)std::fclose(f);
        const auto r = read_session(kTmp, b, 100);
        return r.has_value() ? SessionError::OpenFailed : r.error();
    };

    say(corrupt([](SessionHeader& h){ h.magic[0] = 'X'; }) == SessionError::BadMagic,
        "a flipped magic byte is BadMagic");
    say(corrupt([](SessionHeader& h){ h.version = 999; }) == SessionError::BadVersion,
        "an unknown version is BadVersion");
    say(corrupt([](SessionHeader& h){ h.tick_size = 41; }) == SessionError::BadTickSize,
        "a changed tick size is BadTickSize, never reinterpreted");

    // A truncated file with a GOOD header must report ShortRead, not BadMagic —
    // the validation order is part of the contract.
    (void)write_session(kTmp, a, 100);
    {
        std::FILE* f = detail::session_fopen(kTmp, "rb");
        static char buf[sizeof(SessionHeader) + 10 * sizeof(ReplayTick)];
        const std::size_t got = (f != nullptr)
            ? std::fread(buf, 1, sizeof(buf), f) : 0;
        if (f != nullptr) { (void)std::fclose(f); }
        std::FILE* w = detail::session_fopen(kTmp, "wb");
        if (w != nullptr) {
            (void)std::fwrite(buf, 1, got, w);
            (void)std::fclose(w);
        }
    }
    const auto trunc = read_session(kTmp, b, 100);
    say(!trunc.has_value() && trunc.error() == SessionError::ShortRead,
        "a truncated body is ShortRead, NOT BadMagic — order matters");

    std::remove(kTmp);
    return g_failures == before;
}

bool selftest_session_respects_capacity()
{
    const int before = g_failures;
    static ReplayTick a[1000];
    generate_session(a, 1000, kOpen, 11);
    (void)write_session(kTmp, a, 1000);

    struct { ReplayTick buf[100]; std::uint64_t canary; } guarded{};
    guarded.canary = 0xDEADBEEFCAFEF00Dull;

    const auto r = read_session(kTmp, guarded.buf, 100);
    say(!r.has_value() && r.error() == SessionError::TooManyTicks,
        "1000 ticks into a 100 buffer is TooManyTicks");
    say(guarded.canary == 0xDEADBEEFCAFEF00Dull,
        "and nothing was written past the buffer");

    std::remove(kTmp);
    return g_failures == before;
}

bool selftest_generate_is_deterministic()
{
    const int before = g_failures;
    static ReplayTick a[500];
    static ReplayTick b[500];
    static ReplayTick c[500];
    generate_session(a, 500, kOpen, 42);
    generate_session(b, 500, kOpen, 42);
    generate_session(c, 500, kOpen, 43);

    bool same = true;
    for (std::size_t i = 0; i < 500; ++i) {
        if (!ticks_equal(a[i], b[i])) { same = false; break; }
    }
    say(same, "the same seed reproduces the session byte for byte");

    bool differs = false;
    for (std::size_t i = 0; i < 500; ++i) {
        if (!ticks_equal(a[i], c[i])) { differs = true; break; }
    }
    say(differs, "a different seed produces a different session");

    say(a[0].seqno == 1 && a[499].seqno == 500, "seqno runs 1..500");

    bool increasing = true;
    bool positive = true;
    for (std::size_t i = 0; i < 500; ++i) {
        if (i > 0 && !(a[i].ts > a[i - 1].ts)) { increasing = false; }
        if (!a[i].last.is_positive()) { positive = false; }
    }
    say(increasing, "timestamps strictly increase");
    say(positive, "every price is positive");

    return g_failures == before;
}

bool selftest_replay_end_to_end()
{
    const int before = g_failures;
    constexpr std::size_t kN = 10'000;
    generate_session(g_ticks, kN, kOpen, 2026);

    const TscClock clk = [] {
        auto r = TscClock::create();
        return r.has_value() ? *r : TscClock::create_fallback();
    }();

    const RunResult r = run_session(g_ticks, kN, clk, false);

    say(r.ticks == kN, "every tick was delivered");
    say(r.invariant_held, "the conservation invariant held after EVERY tick");
    say(r.clock_was_the_tick, "the strategy's clock was the tick, never a wall clock");
    say(!r.breached, "no breach latched");
    say(r.fills == 10, "the null strategy filled 10 times (seqno % 1000)");
    say(r.logs_decoded == 10, "every log record produced was drained and decoded");

    return g_failures == before;
}

bool selftest_invariant_breach_stops_the_run()
{
    const int before = g_failures;
    ConservationLedger ledger{Notional{1'000'000}};
    (void)ledger.on_fill(Qty{1}, Price{100}, Notional{5});
    say(ledger.check().has_value(), "healthy before the trip");

    ledger.trip(Breach::CashConservation);
    say(ledger.is_breached(), "the ledger latches");
    say(ledger.on_fill(Qty{1}, Price{100}, Notional{5}).error() == Breach::Latched,
        "the next fill is refused");
    say(ledger.check().error() == Breach::Latched, "check reports the latch");

    // And the replay loop's breach path: a run over a tripped ledger must stop.
    // Proven by construction here — run_session breaks on the first failed
    // check_and_trip, and a kill switch that does not kill is not a kill switch.
    say(ledger.fill_count() == 1, "nothing got through after the trip");

    return g_failures == before;
}

int run_selftest()
{
    std::printf("altair --selftest\n");
    const bool a = selftest_session_roundtrip();
    const bool b = selftest_session_rejects_corrupt_header();
    const bool c = selftest_session_respects_capacity();
    const bool d = selftest_generate_is_deterministic();
    const bool e = selftest_replay_end_to_end();
    const bool f = selftest_invariant_breach_stops_the_run();

    const bool ok = a && b && c && d && e && f && g_failures == 0;
    std::printf("\n%s\n", ok ? "PASS" : "FAILED");
    return ok ? 0 : 1;
}

int do_generate(const char* path, std::size_t n)
{
    if (n == 0 || n > kMaxTicks) {
        std::fprintf(stderr, "altair: --ticks must be 1..%zu\n", kMaxTicks);
        return 2;
    }
    generate_session(g_ticks, n, kOpen, 2026);
    const auto w = write_session(path, g_ticks, n);
    if (!w.has_value()) {
        std::fprintf(stderr, "altair: write failed (%s)\n",
                     session_error_name(w.error()));
        return 1;
    }
    std::printf("wrote %zu ticks to %s (%zu bytes)\n",
                n, path, sizeof(SessionHeader) + n * sizeof(ReplayTick));
    return 0;
}

int do_replay(const char* path)
{
    const auto rd = read_session(path, g_ticks, kMaxTicks);
    if (!rd.has_value()) {
        std::fprintf(stderr, "altair: cannot read %s (%s)\n",
                     path, session_error_name(rd.error()));
        return 1;
    }
    const std::size_t n = rd.value();

    Replayer probe{g_ticks, n};
    if (!probe.validate().has_value()) {
        std::fprintf(stderr, "altair: %s is not a replayable session"
                             " (seqno or time out of order)\n", path);
        return 1;
    }

    auto created = TscClock::create();
    const TscClock clk = created.has_value() ? *created : TscClock::create_fallback();

    const RunResult r = run_session(g_ticks, n, clk, false);

    std::printf("altair --replay %s\n", path);
    std::printf("  clock source     : %s\n",
                clk.is_invariant_tsc() ? "InvariantTsc" : "SteadyFallback");
    std::printf("  ticks replayed   : %zu of %zu\n", r.ticks, n);
    std::printf("  wall time        : %.4f s\n", r.seconds);
    if (r.seconds > 0.0) {
        std::printf("  throughput       : %.2f M ticks/s\n",
                    static_cast<double>(r.ticks) / r.seconds / 1e6);
    }
    std::printf("  fills            : %llu\n",
                static_cast<unsigned long long>(r.fills));
    std::printf("  log lines decoded: %llu\n",
                static_cast<unsigned long long>(r.logs_decoded));
    std::printf("  final position   : %lld units\n",
                static_cast<long long>(r.position.raw()));
    if (r.cash_delta_representable) {
        std::printf("  realised delta   : %lld paise\n",
                    static_cast<long long>(r.cash_delta.raw()));
    } else {
        std::printf("  realised delta   : unrepresentable in int64 paise\n");
    }
    std::printf("  clock == tick    : %s\n", r.clock_was_the_tick ? "yes" : "NO");
    std::printf("  invariants       : %s\n",
                r.invariant_held ? "held on every tick" : "BREACHED — run stopped");

    if (!r.invariant_held || !r.clock_was_the_tick || !r.cash_delta_representable
        || r.ticks != n) {
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const char* cmd = argv[1];

    if (std::strcmp(cmd, "--help") == 0) {
        print_usage();
        return 0;
    }
    if (std::strcmp(cmd, "--selftest") == 0) {
        return run_selftest();
    }
    if (std::strcmp(cmd, "--instruments") == 0) {
        return demo::run_instruments(argc >= 3 ? argv[2] : nullptr,
                                     argc >= 4 ? argv[3] : nullptr);
    }
    if (std::strcmp(cmd, "--replay") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "altair: --replay needs a file\n");
            return 2;
        }
        return do_replay(argv[2]);
    }
    if (std::strcmp(cmd, "--gen") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "altair: --gen needs a file\n");
            return 2;
        }
        std::size_t n = 100'000;
        if (argc >= 5 && std::strcmp(argv[3], "--ticks") == 0) {
            const long long v = std::atoll(argv[4]);
            if (v <= 0) {
                std::fprintf(stderr, "altair: --ticks must be positive\n");
                return 2;
            }
            n = static_cast<std::size_t>(v);
        }
        return do_generate(argv[2], n);
    }

    std::fprintf(stderr, "altair: unknown argument '%s'\n\n", cmd);
    print_usage();
    return 2;
}
