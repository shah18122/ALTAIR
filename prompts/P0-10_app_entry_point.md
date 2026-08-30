# P0-10 — `app/`: the `altair` binary and the Phase 0 exit criterion

> Phase 0 · Card 10 of 14 · Status: DONE (implemented by Claude, 2026-08-31 — see LEDGER)
> Depends on: P0-01 · P0-03 · P0-05a/b · P0-06a · P0-07 · P0-08a · P0-09a/b — all DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** Added to close Phase 0 gate item
> 7: ROADMAP §12's exit criterion is `altair --replay sample.tick` running end to
> end with a null strategy, and there was no binary. Card total 114 → 115; Phase
> 0 goes to 14.
>
> Two decisions. **The session file format is a skeleton, deliberately** — a
> magic, a version, and a flat array of `ReplayTick`. P2-06/07 defines the real
> mmap'd columnar store; this exists so the exit criterion is demonstrable
> *today* and so a real capture drops in behind the same reader. **And the
> binary can generate its own session** (`--gen`), because there is no captured
> tick data yet and the alternative is a criterion nobody can run.
>
> This card is also the first thing that proves the Phase 0 modules compose. It
> wires nine of them together, and a wiring bug that no unit test could see shows
> up here or nowhere.

---

## 1. CONTEXT

You are implementing the entry point of Altair, a C++23 low-latency trading
engine for Indian equity markets.

ROADMAP §12's Phase 0 exit criterion is exact: **`altair --replay sample.tick`
runs end to end with a null strategy; latency harness green; all invariants
armed.** Every piece exists — the replayer, the conservation ledger, the logger,
the config store, the allocators — and none of them has ever been run together.

That is the point of this card. Unit tests prove each module against its own
contract; only an assembled binary proves they compose. A ring sized wrong, a
logger whose consumer never drains, a ledger that is never checked — none of
those fail a unit test.

**The null strategy trades on a fixed schedule and predicts nothing.** It exists
to move the ledger, not to make money. Anything that looks like alpha in Phase 0
is a bug.

---

## 2. FILE MANIFEST

Create exactly these three files, and modify exactly one.

```
CREATE   app/session_file.hpp
CREATE   app/main.cpp
CREATE   app/CMakeLists.txt
MODIFY   CMakeLists.txt          (root — uncomment add_subdirectory(app))
```

In the root `CMakeLists.txt`, change `# add_subdirectory(app)             # main()`
to an active `add_subdirectory(app)`. **Change nothing else in that file.**

`app/CMakeLists.txt` builds an executable named **`altair`** from `main.cpp`,
links `altair_core`, `altair_feed` and `altair_flags`, and under
`if(ALTAIR_BUILD_TESTS)` registers the binary's own self-test with
`add_test(NAME app_selftest COMMAND altair --selftest)`.

**Do not touch** `vcpkg.json`, `core/`, `feed/`, or `config/`.

---

## 3. INTERFACE CONTRACT

### `app/session_file.hpp`

```cpp
#pragma once

#include <feed/replay.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a session file could not be read or written.
// ─────────────────────────────────────────────────────────────────────────
enum class SessionError : std::uint8_t {
    OpenFailed,       // the path could not be opened
    ShortRead,        // the file ended mid-record
    BadMagic,         // not an Altair session file
    BadVersion,       // a format this build does not know
    BadTickSize,      // ReplayTick changed size since the file was written
    TooManyTicks,     // more ticks than the caller's buffer holds
    WriteFailed       // the write did not complete
};

/// Magic at the head of every session file: "ALTAIRTK".
inline constexpr char kSessionMagic[8] = {'A','L','T','A','I','R','T','K'};

/// Format version this build reads and writes. UNIT: none.
inline constexpr std::uint32_t kSessionVersion = 1;

// ─────────────────────────────────────────────────────────────────────────
// SessionHeader — 32 bytes, then `count` ReplayTick records back to back.
//
// A SKELETON format. P2-06/07 defines the real mmap'd columnar store; this
// exists so the Phase 0 exit criterion is runnable and so a real capture
// drops in behind the same reader.
// ─────────────────────────────────────────────────────────────────────────
struct SessionHeader {
    char magic[8];
    /// kSessionVersion at write time. UNIT: none.
    std::uint32_t version;
    /// sizeof(ReplayTick) at write time. UNIT: bytes. A mismatch means the
    /// struct changed and the file cannot be trusted — reject, never reinterpret.
    std::uint32_t tick_size;
    /// Records following the header. UNIT: ticks.
    std::uint64_t count;
    std::uint64_t reserved;
};

/// Write a session. UNIT: none.
/// PRECONDITION: `ticks` holds `count` records; `path` is writable.
/// Returns OpenFailed or WriteFailed. Never partially succeeds silently:
/// a failed write leaves an error, and the caller must not trust the file.
[[nodiscard]] inline std::expected<void, SessionError>
write_session(const char* path, const ReplayTick* ticks, std::size_t count) noexcept;

/// Read a session into a caller-supplied buffer. UNIT: none.
/// PRECONDITION: `out` holds at least `capacity` records.
/// Returns the number of ticks read, or BadMagic / BadVersion / BadTickSize /
/// ShortRead / TooManyTicks / OpenFailed. Allocates nothing.
[[nodiscard]] inline std::expected<std::size_t, SessionError>
read_session(const char* path, ReplayTick* out, std::size_t capacity) noexcept;

/// NOTE the `inline` on both free functions above and on generate_session
/// below: this header is included by more than one translation unit, and a
/// non-inline definition in a header is a duplicate-symbol link error. The
/// card declared them without it, which was under-specified rather than wrong.
/// Fill `out` with a deterministic synthetic session. UNIT: none.
/// PRECONDITION: `out` holds `count` records.
/// The same `seed` and `count` always produce the same session on every box,
/// so a replay is reproducible — it uses a fixed LCG, never std::random.
/// Timestamps start at `start` and advance 1 ms per tick; seqno starts at 1.
/// This exists because there is no captured tick data yet, NOT because
/// synthetic data is a substitute for it.
inline void generate_session(ReplayTick* out, std::size_t count,
                             Timestamp start, std::uint64_t seed) noexcept;

} // namespace altair
```

### `app/main.cpp`

No public contract — it is a `main()`. It must accept exactly these arguments:

```
altair --replay <file>          replay a session with the null strategy
altair --gen <file> [--ticks N] generate a synthetic session (default N = 100000)
altair --selftest               run the built-in checks, exit 0 only if all pass
altair --help                   usage
```

Unknown arguments print usage to `stderr` and exit **2**. A failed replay or a
failed self-test exits **1**. Success exits **0**.

---

## 4. BEHAVIOURAL SPEC

1. `write_session` writes the header then the records with `std::fwrite`, sets
   `magic` to `kSessionMagic`, `version` to `kSessionVersion`, `tick_size` to
   `sizeof(ReplayTick)`, `count` to the record count, and `reserved` to 0.
2. `read_session` validates **in this order**: `OpenFailed`, header `ShortRead`,
   `BadMagic`, `BadVersion`, `BadTickSize`, `TooManyTicks`, then record
   `ShortRead`. Order matters — a truncated file must not be reported as a bad
   magic, and a wrong `tick_size` must be caught before any record is read.
3. `BadTickSize` is not paranoia. `ReplayTick` is 40 bytes today; if a later
   card changes it, every existing file becomes garbage that would otherwise be
   silently reinterpreted. **Reject, never reinterpret.**
4. `read_session` allocates nothing and never reads past `capacity`.
5. `generate_session` uses a fixed LCG seeded from `seed`, so the same inputs
   give the same session on every machine. seqno is `i + 1`; timestamps are
   `start + i ms`; token is a single fixed instrument; price is a bounded random
   walk in paise that never goes below 1; qty is in [1, 100].
6. `--replay` reads the file, `validate()`s it, and runs the null strategy over
   it. It reports, on stdout: tick count, wall time, ticks/second, final
   position, final cash, realised `cash_delta`, fills, and whether the
   conservation invariant held.
7. **The invariant is checked on every tick**, not at the end. A breach stops
   the replay immediately and exits 1 — that is CLAUDE.md rule 9's kill switch
   doing its job, and a run that continues past a breach proves nothing.
8. **The strategy reads time only from `Replayer::now()`.** Never a wall clock.
   The one `steady_clock` use permitted is the outer wall-time measurement for
   the throughput report, which is not a trading decision.
9. The null strategy fills 1 unit at the tick price whenever
   `seqno % 1000 == 0`, alternating buy and sell, with a fixed cost. It predicts
   nothing. **Anything resembling alpha here is a bug.**
10. `--replay` wires up and exercises, at minimum: `TscClock` (falling back to
    `create_fallback()`), a `PageBlock` + `Arena` for scratch, a `SpscRing` +
    `Logger` + `LogDecoder`, a `ConfigStore` + `ConfigSnapshot`, a
    `ConservationLedger`, and the `Replayer`. Log records must be **drained and
    decoded**, not merely produced — a logger whose consumer never runs is a
    ring that silently fills, and no unit test catches it.
11. `--selftest` runs the named checks in §6 and returns 0 only if every one
    passes. It must not touch the network, and any file it writes goes to a
    temporary path it removes afterwards.
12. Nothing in `main.cpp` is `ALTAIR_HOT`. The replay loop calls hot functions;
    it is not itself one.

---

## 5. CONSTRAINTS

- C++23. Standard library plus Altair headers. **No new dependency.**
- File I/O uses `<cstdio>` (`std::fopen`/`fread`/`fwrite`/`fclose`).
  **No `<iostream>`, no `<fstream>`, no `<format>`.** Output is `std::printf`.
- No exceptions, no `throw`. Errors are `std::expected` and exit codes.
- The tick buffer is a single `static` array or a `PageBlock`, **never**
  `std::vector` or `new` — this binary is the demonstration that the engine does
  not allocate at runtime.
- `<chrono>` is permitted in `main.cpp` for the outer wall-time measurement only.
- No `using namespace` at file scope in `session_file.hpp`.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- **Reads no credentials and opens no socket.** Live feeds are Phase 2/4; a
  Phase 0 binary that could reach a broker is a Phase 0 binary that could place
  an order.

---

## 6. ACCEPTANCE TESTS

Implemented inside `main.cpp`, run by `altair --selftest`, and registered with
`add_test(NAME app_selftest COMMAND altair --selftest)`. Use exactly these names:

```cpp
bool selftest_session_roundtrip();
bool selftest_session_rejects_corrupt_header();
bool selftest_session_respects_capacity();
bool selftest_generate_is_deterministic();
bool selftest_replay_end_to_end();
bool selftest_invariant_breach_stops_the_run();
```

Each returns true on success and prints its own failures. `--selftest` runs all
six and exits 0 only if every one returned true.

**selftest_session_roundtrip**
```
Generate 1'000 ticks into a buffer, write to a temp path, read back into a
second buffer.
  read count == 1'000
  every field of every tick is byte-identical to what was written
  (compare ts, seqno, token, last, qty on all 1'000)
Remove the temp file.
```

**selftest_session_rejects_corrupt_header**
```
Write a valid file, then corrupt one field at a time and confirm the exact error:
  magic[0] flipped        -> BadMagic
  version = 999           -> BadVersion
  tick_size = 41          -> BadTickSize
  count = 1'000 but the file truncated after 10 records -> ShortRead
  a path that does not exist -> OpenFailed
The ORDER matters: a truncated file with a good header must report ShortRead,
not BadMagic.
```

**selftest_session_respects_capacity**
```
A 1'000-tick file read into a 100-capacity buffer -> TooManyTicks
and the buffer is NOT written past element 99 (guard it with a canary).
```

**selftest_generate_is_deterministic**
```
generate_session(a, 500, kOpen, 42) and generate_session(b, 500, kOpen, 42)
  -> a and b are byte-identical
generate_session(c, 500, kOpen, 43)
  -> c differs from a           // the seed actually matters
a[0].seqno == 1 and a[499].seqno == 500
a[i].ts is strictly increasing
every a[i].last is a positive Price
```

**selftest_replay_end_to_end**
```
Generate 10'000 ticks, run the null strategy over them in-process:
  every tick delivered
  the conservation invariant held after EVERY tick
  Replayer::now() equalled the delivered tick's ts on every tick
  fills == 10                        // seqno % 1000 == 0, 10'000 ticks
  the ledger is not breached and check() passes at the end
  every log record produced was drained and decoded
```

**selftest_invariant_breach_stops_the_run**
The kill switch, proven rather than assumed.
```
Run a short replay, then trip the ledger deliberately mid-run
(ledger.trip(Breach::CashConservation)) and confirm:
  the next on_fill returns Latched
  check() returns Latched
  the replay loop's breach path is taken            // observe the flag it sets
A breach that does not stop the run is a kill switch that does not kill.
```

### Reporting — not a pass/fail assertion

`--replay` prints a summary. Include ticks/second and the clock source
(`InvariantTsc` or `SteadyFallback`), because a throughput figure from a
fallback clock is not comparable. **No budget is asserted here** — ROADMAP §11
budgets the *stages*, and this loop is a harness around them, not a stage.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Editing the root `CMakeLists.txt` beyond
  the single `add_subdirectory(app)` line. Touching `core/`, `feed/`,
  `config/`, or `vcpkg.json`.
- `<iostream>`, `<fstream>`, `<format>`, `std::vector`, `new`, or `malloc`.
- **Reading any credential, environment variable, or opening any socket.**
  A Phase 0 binary that can reach a broker is one that can place an order.
- Reading a wall clock for any *trading* decision. `Replayer::now()` only.
- Continuing the replay past an invariant breach.
- Checking the invariant only at the end of the run.
- Producing log records without draining them.
- Reinterpreting a session file whose `tick_size` does not match.
- `std::random` in `generate_session` — the session must reproduce byte for byte.
- A null strategy that looks at future ticks, or that appears to make money on
  purpose. It predicts nothing.
- Writing a test that asserts whatever your implementation happens to produce.

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
the ticks/second `--replay` achieved, the clock source it used, and confirm that
the binary opens no socket and reads no credential.
