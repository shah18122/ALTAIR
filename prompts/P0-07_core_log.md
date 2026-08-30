# P0-07 — `core/log`: binary async logger + off-thread decoder

> Phase 0 · Card 7 of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 · P0-03 (`tsc_clock`) · P0-06a (`spsc_ring`) — all DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** Four decisions made here rather
> than left open: (a) **no string arguments** — a string is the allocation vector
> that this whole design exists to avoid, and in Altair symbols are `uint32`
> tokens anyway, so integers cover the real cases; (b) **fixed-size 64-byte
> records**, one cache line, so the transport is the existing `SpscRing` rather
> than a bespoke byte ring; (c) **the hot path stores raw TSC ticks**, and the
> decoder converts to nanoseconds off-thread — storing a converted value would
> put a 128-bit multiply on the tick path for no reason; (d) **no globals and no
> macros in this card.** A global registry brings a static-initialisation-order
> problem and makes the whole thing untestable. The ergonomic macro layer is a
> later card; this one delivers the mechanism.
>
> ROADMAP §11 has no log line. Budget is derived: a log call sits *inside*
> already-budgeted stages (wire decode 1 µs, risk checks 3 µs), so it must be a
> small fraction of one. 40 ns, dominated by the ~12 ns ordered TSC read.

---

## 1. CONTEXT

You are implementing the logger of Altair, a C++23 low-latency trading engine
for Indian equity markets.

`printf` and `iostream` format on the calling thread. Formatting an integer to
text is tens of nanoseconds; a full line with a timestamp is hundreds, sometimes
microseconds when the sink blocks. On a path that runs per tick, that is not a
logger, it is an outage.

The answer is to **move the formatting off the thread that logs**. The hot path
writes a compact binary record — a site identifier plus raw argument bytes —
into a lock-free ring and returns. A separate thread turns records into text,
whenever it gets around to it. This is the Quill / NanoLog shape, and ROADMAP
§7 names it directly.

Two consequences fall out, and both are features. The hot path never touches a
format string, so the cost of a log call is independent of how much it prints.
And when the ring is full the hot path **drops the record and counts it** — it
never blocks, because a logger that can stall the tick path is a worse problem
than a missing log line.

---

## 2. FILE MANIFEST

Create exactly these four files. Nothing else.

```
core/log/binlog.hpp
core/log/decoder.hpp
core/log/CMakeLists.txt
core/log/tests/test_binlog.cpp
```

`core/CMakeLists.txt` discovers `log/` via its `EXISTS` guard — **do not modify
it.** `core/log/CMakeLists.txt` declares `altair_log` as an **INTERFACE** library
(header-only), aliases it `altair::log`, exports
`${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root, links `altair_types`,
`altair_time`, `altair_lockfree` and `altair_flags`, and under
`if(ALTAIR_BUILD_TESTS)` registers `altair_binlog_test` from
`tests/test_binlog.cpp`, linking `Threads::Threads`, with
`add_test(NAME binlog COMMAND altair_binlog_test)`.

**Do not touch** `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`,
or any other `core/` subdirectory.

---

## 3. INTERFACE CONTRACT

### `core/log/binlog.hpp`

```cpp
#pragma once

#include <lockfree/spsc_ring.hpp>
#include <time/tsc_clock.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
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
    [[nodiscard]] std::uint32_t register_site(const LogSite& s) noexcept;

    /// Look a site up. UNIT: none. Returns nullptr for an unknown id.
    /// PRECONDITION: no registration is in flight.
    [[nodiscard]] const LogSite* site(std::uint32_t id) const noexcept;

    /// Sites registered so far. UNIT: count.
    [[nodiscard]] std::uint32_t count() const noexcept;

private:
    LogSite sites_[kMaxSites]{};
    std::uint32_t count_ = 0;
};

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
    Logger(Ring& ring, LogLevel min_level) noexcept;

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
                                        Args... args) noexcept;

    /// Current threshold. UNIT: none.
    [[nodiscard]] LogLevel min_level() const noexcept;

    /// Change the threshold. UNIT: none. PRECONDITION: producer thread only.
    void set_min_level(LogLevel lvl) noexcept;

    /// Records successfully pushed. UNIT: records.
    [[nodiscard]] std::uint64_t written() const noexcept;

    /// Write attempts rejected by a full ring. UNIT: attempts, NOT records —
    /// a caller that spins retrying one record increments this every time, so
    /// the counter (and dropped_before with it) assumes the intended
    /// fire-and-forget use. Non-zero there means the consumer is not keeping
    /// up: size the ring, do not raise the threshold.
    [[nodiscard]] std::uint64_t dropped() const noexcept;

    /// Records rejected by the level filter. UNIT: records. Not a fault.
    [[nodiscard]] std::uint64_t filtered() const noexcept;

private:
    Ring* ring_ = nullptr;
    LogLevel min_ = LogLevel::Info;
    std::uint16_t pending_drops_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t filtered_ = 0;
};

} // namespace altair
```

### `core/log/decoder.hpp`

```cpp
#pragma once

#include <log/binlog.hpp>
#include <time/tsc_clock.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// LogDecoder — turns records into text, OFF the producer thread.
//
// Writes into a caller-supplied buffer. Allocates nothing, so it is safe to
// run on a thread with a fixed budget. Every entry point truncates rather
// than overruns and always NUL-terminates when capacity allows.
// ─────────────────────────────────────────────────────────────────────────
class LogDecoder {
public:
    /// Borrow a registry and a clock. Both must outlive the decoder.
    /// The clock converts LogRecord::tsc_ticks to a UTC instant.
    LogDecoder(const LogRegistry& reg, const TscClock& clk) noexcept;

    /// Substitute the record's arguments into its site's format string.
    /// `{}` placeholders are filled positionally. A placeholder with no
    /// argument becomes `{?}`; arguments with no placeholder are appended
    /// as ` (+N unused)`. Format specs such as `{:.2f}` are NOT supported and
    /// are emitted literally.
    /// UNIT: returns bytes written, excluding the NUL.
    /// PRECONDITION: out has room for cap bytes. Returns 0 if cap == 0 or the
    /// site id is unknown.
    [[nodiscard]] std::size_t decode_message(const LogRecord& r,
                                             char* out, std::size_t cap) const noexcept;

    /// A full line: `<ns> <LEVEL> <file>:<line> <message>`, where <ns> is the
    /// UTC instant in nanoseconds since the Unix epoch and <LEVEL> is a
    /// five-character left-aligned name. When dropped_before is non-zero the
    /// line is prefixed with `[dropped N] `.
    /// UNIT: returns bytes written, excluding the NUL.
    /// PRECONDITION: as decode_message.
    [[nodiscard]] std::size_t decode_line(const LogRecord& r,
                                          char* out, std::size_t cap) const noexcept;

    /// Five-character left-aligned level name: "TRACE", "DEBUG", "INFO ",
    /// "WARN ", "ERROR". UNIT: none. Never returns nullptr.
    [[nodiscard]] static const char* level_name(LogLevel lvl) noexcept;

private:
    const LogRegistry* reg_ = nullptr;
    const TscClock* clk_ = nullptr;
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `sizeof(LogRecord) == 64` and `alignof(LogRecord) >= 8`. The layout in the
   contract already sums to 64; do not add, reorder, or repack fields.
   `std::is_trivially_copyable_v<LogRecord>` must be true — the `SpscRing`
   static-asserts it.
2. `LogRegistry::register_site` appends and returns the new index. At
   `kMaxSites` it returns `kInvalidSite` and changes nothing. `site()` returns
   `nullptr` for `kInvalidSite` or any id `>= count()`.
3. `Logger::write` filters first: if `lvl < min_level()`, increment `filtered_`
   and return false **without reading the clock**. The TSC read is the dominant
   cost, and a filtered call must not pay it.
4. Otherwise it stamps `tsc_ticks` from `cpu::read_tsc_ordered()`, packs the
   arguments, sets `dropped_before` from `pending_drops_`, and pushes.
5. On a successful push: `pending_drops_` resets to 0, `written_` increments.
   On a full ring: `dropped_` increments, `pending_drops_` increments
   **saturating at 65535**, and it returns false. It must never block, never
   retry in a loop, and never overwrite an unconsumed record.
6. Argument packing, per argument, in order:
   - `bool` → `ArgTag::Bool`, payload 0 or 1;
   - any signed integral → `ArgTag::I64`, sign-extended into the slot via
     `static_cast<std::int64_t>` then bit-copied;
   - any unsigned integral → `ArgTag::U64`;
   - `float` or `double` → `ArgTag::F64`, the `double` bit-copied with
     `std::memcpy` or `std::bit_cast`. **Never** via a pointer cast, which is a
     strict-aliasing violation.
   Check `bool` **before** the integral test — `bool` is an integral type and
   would otherwise be tagged `I64`.
7. More than `kMaxLogArgs` arguments: pack the first `kMaxLogArgs`, set
   `kLogFlagArgsTruncated`, and still push. Losing the tail of one line is
   better than losing the line.
8. Unused slots have `ArgTag::None` and a zero payload. `arg_count` counts only
   the packed ones.
9. `decode_message` walks the format string, copying literals and substituting
   at each `{}`. Integers use `std::to_chars`; doubles use `std::to_chars` with
   `std::chars_format::general`. `{}` with no remaining argument emits `{?}`.
   Arguments left over after the last placeholder append ` (+N unused)`.
   A `{` not followed by `}` is emitted literally.
10. Both decode entry points **truncate**, never overrun. They write at most
    `cap - 1` bytes and NUL-terminate, returning the byte count excluding the
    NUL. `cap == 0` returns 0 and writes nothing.
11. `decode_line` converts `tsc_ticks` with the clock:
    `clk.calibration().anchor + clk.ticks_to_duration(tsc_ticks - anchor_ticks)`,
    and prints `ns_since_epoch()`. A record stamped before the clock's anchor
    would underflow the unsigned subtraction; print `0` for that case rather
    than a nonsense instant.
12. Nothing in `decoder.hpp` is `ALTAIR_HOT`. It formats, and formatting is
    exactly what this design moves off the hot path. Nothing in `binlog.hpp`
    formats, allocates, or throws.

---

## 5. CONSTRAINTS

- C++23. Standard library plus P0-01/03/06a headers. **No new dependency.**
- Both files header-only. `<charconv>`, `<cstring>` and `<bit>` are permitted in
  `decoder.hpp`; `binlog.hpp` may use `<cstring>` or `<bit>` for the float
  bit-copy and nothing else beyond its contract includes.
- **No `<cstdio>`, no `<iostream>`, no `<format>`, no `<string>`, no
  `std::vector` anywhere in either header.** The whole point is that formatting
  and allocation do not happen on the logging thread; a `std::string` in the
  decoder would still allocate on the decode thread, which has its own budget.
- No exceptions, no `throw`. No dynamic allocation anywhere in this card.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace
  in a header. Name them distinctly from existing `detail` members
  (`mul_overflows`, `add_overflows_i64`, `is_pow2_size`, `round_up_pow2`).
- No `using namespace` at file scope in a header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()` and must link `Threads::Threads`.

---

## 6. ACCEPTANCE TESTS

`core/log/tests/test_binlog.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_log_record_layout();
void test_log_registry();
void test_log_level_filtering();
void test_log_arg_packing();
void test_log_decode_message();
void test_log_decode_truncation();
void test_log_ring_full_drops_and_marks_gap();
void test_log_two_thread_roundtrip();
```

Build the clock with `TscClock::create()`, falling back to
`TscClock::create_fallback()` when the box has no invariant TSC, exactly as the
P0-03 tests do.

**test_log_record_layout**
```
sizeof(LogRecord) == 64                       // one cache line, exactly
alignof(LogRecord) >= 8
std::is_trivially_copyable_v<LogRecord>
kMaxLogArgs == 5
// The ring must accept it - this is the reason for the size discipline.
SpscRing<LogRecord, 8>::capacity() == 8
```

**test_log_registry**
```
LogRegistry reg;
reg.count() == 0
static const LogSite s1{"a {}", "t.cpp", 10, LogLevel::Info};
const auto id1 = reg.register_site(s1);
id1 == 0 && reg.count() == 1
reg.site(id1)->line == 10
reg.site(id1)->fmt is the same pointer as s1.fmt      // no copy of the text

static const LogSite s2{"b", "t.cpp", 11, LogLevel::Warn};
reg.register_site(s2) == 1 && reg.count() == 2

reg.site(2) == nullptr                                 // beyond count
reg.site(LogRegistry::kInvalidSite) == nullptr
```

**test_log_level_filtering**
```
Ring ring; Logger log{ring, LogLevel::Warn};
!log.write(id, LogLevel::Info, 1)          // below threshold -> false
log.filtered() == 1 && log.written() == 0
ring.size_approx() == 0                     // nothing was pushed

log.write(id, LogLevel::Warn, 1)            // at threshold -> accepted
log.write(id, LogLevel::Error, 2)           // above -> accepted
log.written() == 2 && log.filtered() == 1

log.set_min_level(LogLevel::Trace);
log.write(id, LogLevel::Trace, 3)
log.written() == 3
```

**test_log_arg_packing**
```
Logger at Trace. Push one record per case and pop it back.

write(id, Info, std::int64_t{-42})
    -> arg_count == 1, arg_tags[0] == ArgTag::I64
    -> static_cast<std::int64_t>(args[0]) == -42
write(id, Info, std::uint64_t{42})
    -> ArgTag::U64, args[0] == 42
write(id, Info, 3.5)
    -> ArgTag::F64, bit_cast<double>(args[0]) == 3.5
write(id, Info, true)
    -> ArgTag::Bool, args[0] == 1        // NOT I64 - bool is integral, and
                                          // the bool test must come first
write(id, Info, -7)                       // plain int
    -> ArgTag::I64, static_cast<std::int64_t>(args[0]) == -7

// No args at all.
write(id, Info) -> arg_count == 0, arg_tags[0] == ArgTag::None, args[0] == 0

// Exactly the maximum.
write(id, Info, 1, 2, 3, 4, 5)
    -> arg_count == 5, flags == 0

// One too many: the first five survive, the record is flagged, still pushed.
write(id, Info, 1, 2, 3, 4, 5, 6)
    -> arg_count == 5
    -> (flags & kLogFlagArgsTruncated) != 0
    -> static_cast<std::int64_t>(args[4]) == 5
```

**test_log_decode_message**
Exact strings — this is the specification, not whatever the code emits.
```
site fmt "order {} qty {} px {}"  with args (uint64 7, int64 -50, double 1.5)
    -> "order 7 qty -50 px 1.5"

site fmt "no args here"           with no args
    -> "no args here"

site fmt "flag {}"                with (true)
    -> "flag true"
site fmt "flag {}"                with (false)
    -> "flag false"

// A placeholder with nothing to fill it.
site fmt "a {} b {}"              with (1)
    -> "a 1 b {?}"

// Arguments with nowhere to go.
site fmt "just {}"                with (1, 2, 3)
    -> "just 1 (+2 unused)"

// A lone brace is literal, and a format spec is NOT interpreted.
site fmt "brace { here"           with no args
    -> "brace { here"
site fmt "spec {:.2f}"            with (1.5)
    -> "spec {:.2f} (+1 unused)"
// Note the suffix, and do not "fix" it away. `{:.2f}` is not a placeholder to
// this decoder, so the argument genuinely went unconsumed - and saying so is
// exactly right: it surfaces the unsupported spec at the point of use instead
// of silently swallowing a value the author expected to see printed.

// decode_line ends with the message and names the site.
decode_line(...) for "order {} qty {} px {}" contains "t.cpp:10"
decode_line(...) contains "INFO "
decode_line(...) ends with "order 7 qty -50 px 1.5"
```

**test_log_decode_truncation**
```
char buf[8];
decode_message(rec_with_long_message, buf, sizeof(buf))
    -> return value <= 7
    -> buf[return value] == '\0'          // always terminated
    -> no write past buf[7]                // guard with a canary byte after it

decode_message(rec, buf, 0) == 0           // cap 0 writes nothing
decode_message(rec, buf, 1) == 0 && buf[0] == '\0'

// An unknown site id yields 0, not a crash.
LogRecord bad{}; bad.site_id = 999;
decode_message(bad, buf, sizeof(buf)) == 0
```

**test_log_ring_full_drops_and_marks_gap**
```
SpscRing<LogRecord, 4> ring; Logger log{ring, LogLevel::Trace};

// Fill it.
four writes all succeed; log.written() == 4 && log.dropped() == 0
// Overflow.
!log.write(id, Info, 99)
log.dropped() == 1 && log.written() == 4
!log.write(id, Info, 98)
log.dropped() == 2

// Drain one slot, then write again: the gap is marked IN THE STREAM.
LogRecord r{}; ring.try_pop(r);
log.write(id, Info, 100)
pop until the newest record; its dropped_before == 2
// And the counter resets after a successful write.
ring.try_pop(r) ... ; log.write(id, Info, 101);
newest record's dropped_before == 0

// The saturation is documented, so assert it is a saturation and not a wrap.
// (Drive 70000 drops on a full ring and confirm dropped_before caps at 65535.)
```

**test_log_two_thread_roundtrip**
One producer thread logging **200'000 records**, one consumer decoding them.
```
The producer writes record i with a single uint64 argument == i, spinning on a
full ring (with yield) so nothing is dropped.
The consumer pops 200'000 records and, for each, decodes the message and
verifies the argument round-tripped:
    - args[0] == expected i, in order          // SPSC preserves order
    - decode_message succeeds (returns > 0)

  every argument arrived in order
  every record decoded (decode_message returned > 0)
  log.written() == 200'000
  ring drained
// Report elapsed time, records/second, and log.dropped().
```

**Do not assert `log.dropped() == 0` here.** A spinning producer increments that
counter on every failed attempt even though the retry then delivers the record —
`dropped()` counts **attempts, not losses**. Delivery is proved by the ordering
check and by `written()`. Say so in a comment, because the obvious assertion is
wrong in a way that looks right.

### Latency reporting — not a pass/fail assertion

Batch-timed (the P0-05b technique), single-threaded, consumer draining so the
ring never fills: report ns/call for `write(id, Info, u64, i64, f64)`.

**Budget: < 40 ns per call.** ROADMAP §11 has no log line, so this is derived:
a log call sits *inside* already-budgeted stages (wire decode 1 µs, risk checks
3 µs) and must be a small fraction of one. Roughly 12 ns of it is the ordered
TSC read, which is irreducible.

Also report `decode_line` ns/call — it is off the hot path and has no budget,
but a decoder slower than the producer is a ring that fills. Print both;
**do not assert.**

---

## 7. FORBIDDEN

- Adding a file not in the manifest, or touching another `core/` subdirectory.
- Changing any signature in the interface contract.
- Changing the `LogRecord` layout, or letting `sizeof` differ from 64.
- **A string argument tag, or copying any caller string into a record.** Site
  text is `const char*` with static storage and is never copied; runtime strings
  are out of scope for this card by design.
- `<cstdio>`, `<iostream>`, `<format>`, `<string>`, `std::vector`, or any
  allocation, in either header.
- **Reading the clock before the level filter.** A filtered call must not pay
  the TSC read; spec item 3.
- Blocking, spinning, or retrying inside `write` when the ring is full. Drop and
  count.
- Overwriting an unconsumed record.
- Tagging `bool` as `I64`. Test `bool` first; item 6.
- Reinterpreting a `double` through a pointer cast. Use `std::bit_cast` or
  `std::memcpy`.
- Interpreting format specs (`{:.2f}`). They are emitted literally.
- A decode path that can write past `cap`, or that leaves the buffer
  unterminated.
- Marking anything in `decoder.hpp` as `ALTAIR_HOT`.
- Writing a test that asserts whatever your implementation happens to produce.
  The expected strings in §6 are the specification.

---

## 8. RULES

```
RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No `using namespace` at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If a requirement is ambiguous, implement the most conservative reading and
    list the ambiguity under "ASSUMPTIONS" at the end of your response.
```

Return the four files in full, then your ASSUMPTIONS section, which must state
the measured ns/call for `write` and for `decode_line`, and confirm
`sizeof(LogRecord) == 64` on your compiler.
