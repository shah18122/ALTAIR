#pragma once

// P0-07 — binary async logger: the hot-path writer.
//
// printf and iostream format on the calling thread. Formatting one integer is
// tens of nanoseconds; a full line with a timestamp is hundreds, sometimes
// microseconds when the sink blocks. On a path that runs per tick that is not a
// logger, it is an outage.
//
// So the formatting moves off the thread that logs. The hot path writes a
// compact binary record — a site id plus raw argument bytes — into an SpscRing
// and returns. core/log/decoder.hpp turns records into text on another thread.
// (Quill / NanoLog shape; ROADMAP §7 names it.)
//
// Two consequences, both features: the cost of a log call is independent of how
// much it will eventually print, and when the ring is full the hot path DROPS
// and COUNTS rather than blocking — a logger that can stall the tick path is a
// worse problem than a missing line.
//
// Nothing here formats, allocates, or throws.

#include <lockfree/spsc_ring.hpp>
#include <time/tsc_clock.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Severity. Ordered: a Logger drops anything below its minimum.
// ─────────────────────────────────────────────────────────────────────────
enum class LogLevel : std::uint8_t {
    Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4
};

// ─────────────────────────────────────────────────────────────────────────
// What an argument slot holds. There is deliberately NO string tag:
// a string argument is the allocation this design exists to avoid, and in
// Altair a symbol is a uint32 token, not text.
// ─────────────────────────────────────────────────────────────────────────
enum class ArgTag : std::uint8_t {
    None = 0, I64 = 1, U64 = 2, F64 = 3, Bool = 4
};

/// Argument slots in one record. UNIT: count.
inline constexpr std::size_t kMaxLogArgs = 5;

// ─────────────────────────────────────────────────────────────────────────
// LogRecord — exactly one cache line. Trivially copyable, so it rides the
// SpscRing from P0-06a with no bespoke transport.
// ─────────────────────────────────────────────────────────────────────────
struct LogRecord {
    /// Raw tick counter at the moment of the call. UNIT: TSC ticks.
    /// NOT nanoseconds: the conversion is a 128-bit multiply and belongs
    /// off-thread. The decoder converts.
    std::uint64_t tsc_ticks;

    /// Index into the LogRegistry that produced this record. UNIT: none.
    std::uint32_t site_id;

    /// Severity as recorded at the call site. UNIT: none.
    LogLevel level;

    /// Argument slots actually used, 0..kMaxLogArgs. UNIT: count.
    std::uint8_t arg_count;

    /// Bit 0: the call passed more than kMaxLogArgs arguments and the excess
    /// was dropped. UNIT: bitfield.
    std::uint8_t flags;

    std::uint8_t reserved0;

    /// Type of each argument slot. UNIT: none.
    ArgTag arg_tags[kMaxLogArgs];

    std::uint8_t reserved1;

    /// Records lost to a full ring since the last successful write, saturating
    /// at 65535. UNIT: records. Non-zero marks a gap exactly where it happened,
    /// which a separate counter could not.
    std::uint16_t dropped_before;

    /// Raw argument payloads. Reinterpreted per the matching arg_tags entry.
    /// UNIT: none.
    std::uint64_t args[kMaxLogArgs];
};

static_assert(sizeof(LogRecord) == 64, "LogRecord must be exactly one cache line");
static_assert(alignof(LogRecord) >= 8, "LogRecord must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<LogRecord>,
              "LogRecord rides an SpscRing, which requires trivial copyability");

/// Bit 0 of LogRecord::flags.
inline constexpr std::uint8_t kLogFlagArgsTruncated = 0x01;

// ─────────────────────────────────────────────────────────────────────────
// LogSite — one call site. Registered once; the hot path carries only its id.
// All pointers must have static storage duration and outlive the registry.
// ─────────────────────────────────────────────────────────────────────────
struct LogSite {
    /// Format string with `{}` placeholders. PRECONDITION: static storage.
    const char* fmt;
    /// Source file. PRECONDITION: static storage.
    const char* file;
    /// Source line. UNIT: none.
    std::uint32_t line;
    /// Severity declared at the site.
    LogLevel level;
};

// ─────────────────────────────────────────────────────────────────────────
// LogRegistry — site table. An ordinary object, NOT a global: a global would
// bring a static-initialisation-order problem and make this untestable.
// Registration is not thread-safe; register every site before starting the
// producer threads.
// ─────────────────────────────────────────────────────────────────────────
class LogRegistry {
public:
    static constexpr std::uint32_t kMaxSites = 1024;
    static constexpr std::uint32_t kInvalidSite = 0xFFFF'FFFFu;

    constexpr LogRegistry() noexcept = default;

    LogRegistry(const LogRegistry&) = delete;
    LogRegistry& operator=(const LogRegistry&) = delete;

    /// Add a site and return its id. UNIT: none.
    /// Returns kInvalidSite when the table is full — never overwrites, never
    /// grows, never allocates.
    /// PRECONDITION: not called concurrently with itself or with site().
    [[nodiscard]] std::uint32_t register_site(const LogSite& s) noexcept {
        if (count_ >= kMaxSites) {
            return kInvalidSite;
        }
        const std::uint32_t id = count_;
        sites_[id] = s;
        ++count_;
        return id;
    }

    /// Look a site up. UNIT: none. Returns nullptr for an unknown id.
    /// PRECONDITION: no registration is in flight.
    [[nodiscard]] const LogSite* site(std::uint32_t id) const noexcept {
        return id < count_ ? &sites_[id] : nullptr;
    }

    /// Sites registered so far. UNIT: count.
    [[nodiscard]] std::uint32_t count() const noexcept { return count_; }

private:
    LogSite sites_[kMaxSites]{};
    std::uint32_t count_ = 0;
};

namespace detail {

/// Pack one argument into the next free slot. Silently ignores anything past
/// kMaxLogArgs — the caller sets the truncation flag.
/// UNIT: none. PRECONDITION: T is arithmetic.
template <typename T>
ALTAIR_HOT void pack_log_arg(LogRecord& r, std::size_t& n, T v) noexcept {
    static_assert(std::is_arithmetic_v<T>,
                  "log arguments must be integral, floating-point, or bool — "
                  "there is deliberately no string argument");
    if (n >= kMaxLogArgs) {
        return;
    }
    const std::size_t i = n++;

    // bool FIRST: it is an integral type and would otherwise be tagged I64.
    if constexpr (std::is_same_v<T, bool>) {
        r.arg_tags[i] = ArgTag::Bool;
        r.args[i] = v ? 1ull : 0ull;
    } else if constexpr (std::is_floating_point_v<T>) {
        r.arg_tags[i] = ArgTag::F64;
        const double d = static_cast<double>(v);
        std::uint64_t bits = 0;
        // memcpy, never a pointer cast — that would be a strict-aliasing
        // violation the optimiser is entitled to exploit.
        std::memcpy(&bits, &d, sizeof(bits));
        r.args[i] = bits;
    } else if constexpr (std::is_signed_v<T>) {
        r.arg_tags[i] = ArgTag::I64;
        const auto s = static_cast<std::int64_t>(v);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &s, sizeof(bits));
        r.args[i] = bits;
    } else {
        r.arg_tags[i] = ArgTag::U64;
        r.args[i] = static_cast<std::uint64_t>(v);
    }
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// Logger — the hot-path writer. ONE PER PRODUCER THREAD: it drives an
// SpscRing, which permits exactly one producer.
// ─────────────────────────────────────────────────────────────────────────
template <std::size_t Capacity>
class Logger {
public:
    using Ring = SpscRing<LogRecord, Capacity>;

    /// Borrow a ring. UNIT: none. The ring must outlive the Logger.
    /// PRECONDITION: this Logger is the ring's only producer.
    Logger(Ring& ring, LogLevel min_level) noexcept
        : ring_(&ring), min_(min_level) {}

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// Record one event. UNIT: none.
    /// Returns false iff the record was dropped — either filtered by level or
    /// lost to a full ring. NEVER blocks, allocates, formats, or throws.
    /// Arguments must be integral, floating-point, or bool; more than
    /// kMaxLogArgs are dropped and the record is flagged.
    /// PRECONDITION: called from exactly one thread, for the Logger's life.
    template <typename... Args>
    [[nodiscard]] ALTAIR_HOT bool write(std::uint32_t site_id,
                                        LogLevel lvl,
                                        Args... args) noexcept {
        // Filter BEFORE reading the clock. The ordered TSC read is the
        // dominant cost of this function and a filtered call must not pay it.
        if (static_cast<std::uint8_t>(lvl) < static_cast<std::uint8_t>(min_)) {
            ++filtered_;
            return false;
        }

        LogRecord r{};
        r.tsc_ticks = cpu::read_tsc_ordered();
        r.site_id = site_id;
        r.level = lvl;
        r.dropped_before = pending_drops_;

        std::size_t n = 0;
        (detail::pack_log_arg(r, n, args), ...);
        r.arg_count = static_cast<std::uint8_t>(n);
        if constexpr (sizeof...(Args) > kMaxLogArgs) {
            // Losing the tail of one line beats losing the line.
            r.flags = static_cast<std::uint8_t>(r.flags | kLogFlagArgsTruncated);
        }

        if (!ring_->try_push(r)) {
            ++dropped_;
            if (pending_drops_ < 0xFFFFu) {
                ++pending_drops_;   // saturating, never wrapping
            }
            return false;
        }

        pending_drops_ = 0;
        ++written_;
        return true;
    }

    /// Current threshold. UNIT: none.
    [[nodiscard]] LogLevel min_level() const noexcept { return min_; }

    /// Change the threshold. UNIT: none. PRECONDITION: producer thread only.
    void set_min_level(LogLevel lvl) noexcept { min_ = lvl; }

    /// Records successfully pushed. UNIT: records.
    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }

    /// Write attempts rejected by a full ring. UNIT: attempts, NOT records.
    ///
    /// The distinction matters: a caller that spins retrying the same record
    /// increments this on every failed attempt, so under a retry loop this is
    /// an attempt count, not a loss count. The counter — and dropped_before
    /// with it — assumes the intended fire-and-forget use, where a false return
    /// means the record is gone.
    ///
    /// Non-zero under fire-and-forget means the consumer is not keeping up:
    /// size the ring, do not raise the threshold.
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

    /// Records rejected by the level filter. UNIT: records. Not a fault.
    [[nodiscard]] std::uint64_t filtered() const noexcept { return filtered_; }

private:
    Ring* ring_ = nullptr;
    LogLevel min_ = LogLevel::Info;
    std::uint16_t pending_drops_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t filtered_ = 0;
};

} // namespace altair
