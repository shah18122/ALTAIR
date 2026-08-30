# P0-08a — `core/config`: versioned, hashed, hot-swappable config store

> Phase 0 · Card 8a of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 · P0-06b (`seqlock`) — both DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** P0-08 as scoped in the LEDGER —
> "hot-reloadable TOML, versioned + hashed" — is split, for two reasons.
>
> The practical one: **there is no vcpkg on the dev box.** `VCPKG_ROOT` is unset,
> vcpkg is not installed, and `builtin-baseline` in `vcpkg.json` is unpinned.
> A card requiring `tomlplusplus` could not be built or tested here, so gates 1
> and 4 would be marked green on faith. **P0-08b — the TOML binding — is
> therefore BLOCKED**, and recorded as a blocker rather than quietly skipped.
>
> The architectural one, which stands regardless: the config *mechanism*
> — atomic snapshot swap, version counter, content hash, typed lookup — has
> nothing to do with the file format. Splitting at that seam means this half
> needs no dependency at all, and it means replay can feed config from a
> captured snapshot instead of re-parsing a file, which CLAUDE.md rule 6
> requires anyway.
>
> Card total 112 → 113; Phase 0 goes to 12.

---

## 1. CONTEXT

You are implementing the configuration store of Altair, a C++23 low-latency
trading engine for Indian equity markets.

Two hard requirements shape this, and both come from CLAUDE.md.

**Rule 10 — every live decision must be reproducible** from
`{model_hash, feature_version, config_hash, spec_version, tick_seqno}`. So a
snapshot is not just a bag of values: it carries a **version** and a **content
hash**, and the hash must be stable. If reading the same file twice produced two
different hashes, the reproducibility claim is worthless.

**Rule 4 — no allocation on the hot path**, and config is read *on* the hot path:
thresholds, limits, sizes. So the store allocates nothing, ever, and a strategy
thread reads from its **own private copy**, refreshed only when the version
actually changes. In the steady state a refresh is one atomic load and a compare.

Config also changes mid-session — a limit tightened, a strategy disabled — and a
reader must never observe a half-applied change. That is exactly the problem
P0-06b's seqlock solves, so this card uses it rather than inventing a second
mechanism.

P0-08b will parse TOML into a snapshot. Nothing here knows what a file is.

---

## 2. FILE MANIFEST

Create exactly these four files. Nothing else.

```
core/config/config.hpp
core/config/store.hpp
core/config/CMakeLists.txt
core/config/tests/test_config.cpp
```

`core/CMakeLists.txt` discovers `config/` via its `EXISTS` guard — **do not
modify it.** `core/config/CMakeLists.txt` declares `altair_config` as an
**INTERFACE** library, aliases it `altair::config`, exports
`${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root, links `altair_types`,
`altair_lockfree` and `altair_flags`, and under `if(ALTAIR_BUILD_TESTS)`
registers `altair_config_test` from `tests/test_config.cpp`, linking
`Threads::Threads`, with `add_test(NAME config COMMAND altair_config_test)`.

**Add no dependency.** `vcpkg.json` stays empty — this card needs only the
standard library. **Do not touch** `core/CMakeLists.txt`, the root
`CMakeLists.txt`, `vcpkg.json`, or any other `core/` subdirectory.

---

## 3. INTERFACE CONTRACT

### `core/config/config.hpp`

```cpp
#pragma once

#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a config operation failed. There is deliberately no "return a default"
// path: a missing or mistyped key is a configuration bug, and CLAUDE.md rule 9
// says fail loud rather than guess.
// ─────────────────────────────────────────────────────────────────────────
enum class ConfigError : std::uint8_t {
    NotFound,     // no such key
    WrongType,    // the key exists but holds another type
    Full,         // the snapshot is at kMaxConfigEntries
    KeyTooLong,   // key exceeds kMaxConfigKeyLen
    EmptyKey      // a null or zero-length key
};

/// What an entry holds.
enum class ConfigType : std::uint8_t { None = 0, Int = 1, Real = 2, Bool = 3 };

/// Entries in one snapshot. UNIT: count.
inline constexpr std::size_t kMaxConfigEntries = 128;

/// Longest key, excluding the NUL. UNIT: bytes.
inline constexpr std::size_t kMaxConfigKeyLen = 47;

// ─────────────────────────────────────────────────────────────────────────
// One key/value pair. Trivially copyable: a snapshot rides a seqlock.
//
// The key text is stored ALONGSIDE its hash, and lookup compares both. The
// hash alone would be a silent-collision bug: two distinct keys mapping to one
// entry would make a strategy read someone else's limit, and nothing would say so.
// ─────────────────────────────────────────────────────────────────────────
struct ConfigEntry {
    /// FNV-1a 64 of the key. UNIT: none. Used only as a fast reject.
    std::uint64_t key_hash;
    /// Raw payload: int64, double, or bool, bit-cast per `type`. UNIT: none.
    std::uint64_t bits;
    /// What `bits` means.
    ConfigType type;
    std::uint8_t reserved[7];
    /// NUL-terminated key text. UNIT: none.
    char key[kMaxConfigKeyLen + 1];
};

// ─────────────────────────────────────────────────────────────────────────
// An opaque, resolved position in a snapshot. Resolve once at startup, read
// on every tick: lookup by key is a linear scan, by handle it is an index.
// A handle is only valid for the snapshot that produced it.
// ─────────────────────────────────────────────────────────────────────────
class ConfigHandle {
public:
    static constexpr std::uint32_t kInvalid = 0xFFFF'FFFFu;

    constexpr ConfigHandle() noexcept = default;
    constexpr explicit ConfigHandle(std::uint32_t idx) noexcept;

    /// UNIT: none. False for a default-constructed handle.
    [[nodiscard]] constexpr bool valid() const noexcept;

    /// UNIT: none. kInvalid when !valid().
    [[nodiscard]] constexpr std::uint32_t index() const noexcept;

    friend constexpr bool operator==(ConfigHandle, ConfigHandle) noexcept = default;

private:
    std::uint32_t idx_ = kInvalid;
};

/// FNV-1a 64 of a NUL-terminated string. UNIT: none. PRECONDITION: s non-null.
[[nodiscard]] constexpr std::uint64_t fnv1a64(const char* s) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// ConfigSnapshot — an immutable-by-convention set of values, plus the identity
// that makes a decision reproducible. Trivially copyable and allocation-free.
// ─────────────────────────────────────────────────────────────────────────
class ConfigSnapshot {
public:
    constexpr ConfigSnapshot() noexcept = default;

    // ── Build side. Not hot; called by a loader, not a strategy. ──

    /// Insert or overwrite. UNIT: none.
    /// Returns EmptyKey, KeyTooLong, or Full. Overwriting an existing key
    /// REPLACES its type as well as its value.
    [[nodiscard]] std::expected<void, ConfigError>
    set_int(const char* key, std::int64_t v) noexcept;

    [[nodiscard]] std::expected<void, ConfigError>
    set_real(const char* key, double v) noexcept;

    [[nodiscard]] std::expected<void, ConfigError>
    set_bool(const char* key, bool v) noexcept;

    // ── Read side. ──

    /// Resolve a key to a handle. UNIT: none. Linear scan — do this once at
    /// startup and keep the handle. Returns NotFound.
    /// PRECONDITION: key non-null.
    [[nodiscard]] ALTAIR_HOT std::expected<ConfigHandle, ConfigError>
    find(const char* key) const noexcept;

    /// UNIT: none. Returns NotFound for an invalid or out-of-range handle,
    /// WrongType when the entry is not an Int.
    [[nodiscard]] ALTAIR_HOT std::expected<std::int64_t, ConfigError>
    get_int(ConfigHandle h) const noexcept;

    [[nodiscard]] ALTAIR_HOT std::expected<double, ConfigError>
    get_real(ConfigHandle h) const noexcept;

    [[nodiscard]] ALTAIR_HOT std::expected<bool, ConfigError>
    get_bool(ConfigHandle h) const noexcept;

    /// Type at a handle. UNIT: none. ConfigType::None for an invalid handle.
    [[nodiscard]] ConfigType type_of(ConfigHandle h) const noexcept;

    /// Entries stored. UNIT: count.
    [[nodiscard]] std::size_t size() const noexcept;

    // ── Identity. CLAUDE.md rule 10. ──

    /// Hash over every key and value. UNIT: none.
    /// ORDER-INDEPENDENT: two snapshots with identical content hash the same
    /// regardless of the order the keys were set. Computed on demand, O(size()).
    [[nodiscard]] std::uint64_t content_hash() const noexcept;

    /// Monotonic generation, assigned by whoever built this snapshot.
    /// UNIT: none. Zero for a default-constructed snapshot.
    [[nodiscard]] std::uint64_t version() const noexcept;

    void set_version(std::uint64_t v) noexcept;

    /// Drop every entry. UNIT: none. Does NOT reset the version.
    void clear() noexcept;

private:
    ConfigEntry entries_[kMaxConfigEntries]{};
    std::uint32_t count_ = 0;
    std::uint32_t reserved_ = 0;
    std::uint64_t version_ = 0;
};

} // namespace altair
```

### `core/config/store.hpp`

```cpp
#pragma once

#include <config/config.hpp>
#include <lockfree/seqlock.hpp>
#include <types/units.hpp>

#include <atomic>
#include <cstdint>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// ConfigStore — one writer publishes snapshots, many readers keep private
// copies. Built on P0-06b's seqlock rather than a second mechanism.
//
// A reader NEVER reads through the store on the hot path. It calls refresh()
// on its own copy, which in the steady state is one atomic load and a compare,
// and only copies when the version actually moved.
// ─────────────────────────────────────────────────────────────────────────
class ConfigStore {
public:
    ConfigStore() noexcept = default;

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    /// Publish a new snapshot. WRITER THREAD ONLY — exactly one, for the life
    /// of the store. UNIT: none. Never blocks.
    /// PRECONDITION: s.version() is greater than the currently published
    /// version, or readers will not notice the change.
    void publish(const ConfigSnapshot& s) noexcept;

    /// Bring `local` up to date. UNIT: none.
    /// Returns true iff `local` was replaced. When the published version equals
    /// local.version() this does NO copy and returns false — that is the
    /// steady-state path and it is one acquire load.
    /// PRECONDITION: `local` is this thread's own copy, not shared.
    [[nodiscard]] ALTAIR_HOT bool refresh(ConfigSnapshot& local) const noexcept;

    /// Currently published version. UNIT: none. Zero before the first publish.
    [[nodiscard]] std::uint64_t version() const noexcept;

    /// Successful publishes since construction. UNIT: count.
    [[nodiscard]] std::uint64_t publishes() const noexcept;

private:
    SeqlockSnapshot<ConfigSnapshot> snap_{};
    std::atomic<std::uint64_t> version_{0};
    std::atomic<std::uint64_t> publishes_{0};
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `fnv1a64` is standard FNV-1a: basis `14695981039346656037u`, prime
   `1099511628211u`, XOR-then-multiply per byte. It must be `constexpr`.
2. `set_*` rejects a null or empty key with `EmptyKey`, a key longer than
   `kMaxConfigKeyLen` with `KeyTooLong`, and a new key at
   `kMaxConfigEntries` with `Full`. **A rejected set changes nothing** —
   not `size()`, not any entry.
3. Setting an **existing** key overwrites its value *and its type*, and does not
   change `size()`. Overwriting at capacity therefore succeeds.
4. `find` compares the hash first as a fast reject, then `strcmp`s the key text.
   **The hash alone is not sufficient**: a collision would silently hand a
   strategy someone else's value.
5. `get_int` returns `WrongType` on a Real or Bool entry, and `NotFound` for an
   invalid handle or one whose index is `>= size()`. Same shape for the others.
   `get_bool` on an Int is `WrongType` — bool and int are not interchangeable
   here even though both fit in the payload.
6. Values round-trip exactly: an `std::int64_t` through `set_int`/`get_int` is
   bit-identical, including `INT64_MIN`; a `double` through `set_real`/`get_real`
   is bit-identical, including a negative value. Use `std::bit_cast` or
   `std::memcpy`, **never a pointer cast**.
7. `content_hash()` is **order-independent**. Combine per-entry hashes with a
   commutative operation (wrapping addition of a mixed
   `key_hash`/`bits`/`type` word). Two snapshots built with the same pairs in a
   different order must hash identically; changing any value or key must change
   the hash. **Order dependence here would break CLAUDE.md rule 10**: the same
   file read twice could yield two hashes, and "reproducible from config_hash"
   would be false.
8. `content_hash()` ignores `version()`. The version says *when*; the hash says
   *what*. Two snapshots with identical content and different versions have the
   same hash, and that is the point — it is how you detect a no-op reload.
9. `ConfigSnapshot` must be trivially copyable — `SeqlockSnapshot` static-asserts
   it — and must allocate nothing.
10. `ConfigStore::publish` stores the snapshot through the seqlock, then
    publishes the version with **release** ordering, then increments
    `publishes_`. The version must become visible only after the payload.
11. `ConfigStore::refresh` loads the version with **acquire**; if it equals
    `local.version()` it returns false **without copying**. Otherwise it copies
    under the seqlock, retrying until clean, and returns true.
12. `version()` is zero before the first publish. `refresh` against a
    never-published store leaves a default-constructed `local` untouched and
    returns false, because both versions are zero.

---

## 5. CONSTRAINTS

- C++23. Standard library plus P0-01 and P0-06b headers. **No new dependency**;
  `vcpkg.json` stays empty.
- Both files header-only. `<bit>` and `<cstring>` are permitted for the payload
  bit-copy and the key compare.
- **No `<string>`, no `std::string_view` in a stored member, no `std::vector`,
  no `std::map`, no allocation anywhere.** The key is a fixed `char` array
  precisely so a snapshot can be memcpy'd through a seqlock.
- No exceptions, no `throw`, no `iostream`, no file I/O. **Nothing here knows
  what a file is** — that is P0-08b.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace
  in a header. Name them distinctly from the existing `detail` members
  (`mul_overflows`, `add_overflows_i64`, `is_pow2_size`, `round_up_pow2`,
  `pack_log_arg`, `LogTextWriter`, `append_log_arg`).
- No `using namespace` at file scope in a header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()` and must link `Threads::Threads`.

---

## 6. ACCEPTANCE TESTS

`core/config/tests/test_config.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_config_set_and_get();
void test_config_type_safety();
void test_config_key_limits_and_capacity();
void test_config_overwrite_semantics();
void test_config_content_hash_order_independent();
void test_config_snapshot_traits();
void test_config_store_publish_and_refresh();
void test_config_store_concurrent_readers();
```

**test_config_set_and_get**
```
ConfigSnapshot c;
c.size() == 0 && c.version() == 0

c.set_int("risk.max_lots", 40).has_value()
c.set_real("risk.kelly_fraction", 0.25).has_value()
c.set_bool("strategy.enabled", true).has_value()
c.size() == 3

c.get_int(c.find("risk.max_lots").value()).value() == 40
c.get_real(c.find("risk.kelly_fraction").value()).value() == 0.25
c.get_bool(c.find("strategy.enabled").value()).value() == true

// Exact round-trips at the extremes, via bit_cast not a pointer cast.
c.set_int("edge.min", std::numeric_limits<std::int64_t>::min());
c.get_int(c.find("edge.min").value()).value()
    == std::numeric_limits<std::int64_t>::min()
c.set_real("edge.neg", -1.5e-300);
c.get_real(c.find("edge.neg").value()).value() == -1.5e-300

c.find("nope").error() == ConfigError::NotFound
```

**test_config_type_safety**
No silent coercion — a mistyped read is a bug, not a conversion.
```
ConfigSnapshot c;
c.set_int("i", 1); c.set_real("r", 1.0); c.set_bool("b", true);

c.get_real(c.find("i").value()).error() == ConfigError::WrongType
c.get_bool(c.find("i").value()).error() == ConfigError::WrongType   // NOT 1->true
c.get_int (c.find("r").value()).error() == ConfigError::WrongType
c.get_int (c.find("b").value()).error() == ConfigError::WrongType   // NOT true->1

c.type_of(c.find("i").value()) == ConfigType::Int
c.type_of(c.find("r").value()) == ConfigType::Real
c.type_of(c.find("b").value()) == ConfigType::Bool
c.type_of(ConfigHandle{}) == ConfigType::None

// An invalid or stale handle is NotFound, never a read of entry 0.
c.get_int(ConfigHandle{}).error() == ConfigError::NotFound
c.get_int(ConfigHandle{9999}).error() == ConfigError::NotFound
```

**test_config_key_limits_and_capacity**
```
ConfigSnapshot c;
c.set_int(nullptr, 1).error() == ConfigError::EmptyKey
c.set_int("", 1).error()      == ConfigError::EmptyKey

char longest[kMaxConfigKeyLen + 1]; fill with 'k'; NUL-terminate
c.set_int(longest, 1).has_value()                 // exactly at the limit
char toolong[kMaxConfigKeyLen + 2]; fill with 'k'; NUL-terminate
c.set_int(toolong, 1).error() == ConfigError::KeyTooLong
c.size() == 1                                      // the rejection changed nothing

// Fill to capacity with distinct keys, then overflow.
ConfigSnapshot f;
128 distinct keys all set successfully; f.size() == kMaxConfigEntries
f.set_int("one_more", 1).error() == ConfigError::Full
f.size() == kMaxConfigEntries                      // unchanged

// Overwriting AT capacity still works - it is not a new entry.
f.set_int("key_0", 999).has_value()
f.size() == kMaxConfigEntries
f.get_int(f.find("key_0").value()).value() == 999
```

**test_config_overwrite_semantics**
```
ConfigSnapshot c;
c.set_int("x", 1);
c.size() == 1
c.set_int("x", 2);
c.size() == 1                                      // still one entry
c.get_int(c.find("x").value()).value() == 2

// Overwriting REPLACES the type.
c.set_real("x", 3.5);
c.size() == 1
c.type_of(c.find("x").value()) == ConfigType::Real
c.get_real(c.find("x").value()).value() == 3.5
c.get_int(c.find("x").value()).error() == ConfigError::WrongType

c.clear();
c.size() == 0
c.find("x").error() == ConfigError::NotFound
```

**test_config_content_hash_order_independent**
The one that protects rule 10.
```
ConfigSnapshot a, b;
a.set_int("alpha", 1); a.set_real("beta", 2.5); a.set_bool("gamma", true);
b.set_bool("gamma", true); b.set_real("beta", 2.5); b.set_int("alpha", 1);
a.content_hash() == b.content_hash()      // SAME CONTENT, DIFFERENT ORDER

// The version is identity, not content.
a.set_version(7); b.set_version(99);
a.content_hash() == b.content_hash()

// Any content change moves the hash.
ConfigSnapshot d; d.set_int("alpha", 1); d.set_real("beta", 2.5);
d.set_bool("gamma", false);               // one bool flipped
d.content_hash() != a.content_hash()

ConfigSnapshot e; e.set_int("alphaX", 1); e.set_real("beta", 2.5);
e.set_bool("gamma", true);                // one key renamed
e.content_hash() != a.content_hash()

// An empty snapshot has a stable hash, and a non-empty one differs from it.
ConfigSnapshot z1, z2;
z1.content_hash() == z2.content_hash()
z1.content_hash() != a.content_hash()

// Type is part of the content: same key, same bits, different type.
ConfigSnapshot t1, t2;
t1.set_int("k", 1);
t2.set_bool("k", true);                   // both would store bits == 1
t1.content_hash() != t2.content_hash()
```

**test_config_snapshot_traits**
```
std::is_trivially_copyable_v<ConfigSnapshot>     // the seqlock requires it
std::is_trivially_copyable_v<ConfigEntry>
kMaxConfigEntries == 128
kMaxConfigKeyLen == 47
// A snapshot is big; state its size so a future change is visible.
// Print sizeof(ConfigSnapshot) - do not assert an exact number.
```

**test_config_store_publish_and_refresh**
```
ConfigStore store;
store.version() == 0 && store.publishes() == 0

ConfigSnapshot local;                     // default: version 0
!store.refresh(local)                     // nothing published, no copy, false
local.size() == 0

ConfigSnapshot s; s.set_int("a", 1); s.set_version(1);
store.publish(s);
store.version() == 1 && store.publishes() == 1

store.refresh(local)                      // true: the version moved
local.version() == 1
local.get_int(local.find("a").value()).value() == 1

!store.refresh(local)                     // second refresh is a no-op
local.version() == 1

// A second publish is picked up, and the OLD value is gone.
ConfigSnapshot s2; s2.set_int("a", 2); s2.set_int("b", 3); s2.set_version(2);
store.publish(s2);
store.refresh(local)
local.version() == 2 && local.size() == 2
local.get_int(local.find("a").value()).value() == 2

// content_hash travels with the snapshot.
local.content_hash() == s2.content_hash()
```

**test_config_store_concurrent_readers**
One writer publishing, **two** readers refreshing. Same construction as the
P0-06b seqlock test — and for the same reason.
```
The writer publishes snapshots with version n and a CONSISTENT payload:
    "n" -> n, "double_n" -> 2n, "flag" -> (n % 2 == 0)
so any observed snapshot must satisfy double_n == 2 * n and flag == (n even).
Each reader refreshes into its own local copy and, whenever refresh() returns
true, checks that invariant.

**The reader MUST yield when refresh() returns false.** An unchanged refresh
is 0.63 ns while a publish copies a 9 KB snapshot, so a hot-spinning reader
burns the whole attempt cap before a loaded writer can produce the target
number of versions -- 4 failures in 6 loaded passes. Yielding makes an
attempt a real scheduling opportunity rather than a spin iteration, which is
also what an actual reader would do. Verified 10 loaded runs, zero failures.

Each reader runs until it has seen kTargetChanges (1'000) DISTINCT versions,
with an attempt cap as a loud backstop. Terminating on the evidence, not on a
timer, is what stopped the P0-06b test passing vacuously - do the same here.

  no inconsistent snapshot was ever observed        // THE assertion
  neither reader hit the attempt cap
  each reader saw >= 1'000 distinct versions
  every observed version was one the writer actually published
// Report versions published and refreshes performed.
```

### Latency reporting — not a pass/fail assertion

Batch-timed (the P0-05b technique):
- `ConfigStore::refresh` on an **unchanged** store — the steady-state hot path.
  **Budget: < 5 ns.** It is one acquire load and a compare; anything more means
  a copy is happening when it should not.
- `ConfigSnapshot::find` on a 128-entry snapshot, worst case (last key) — this
  is why handles exist. No budget; print it next to the handle-based
  `get_int` so the difference is visible.
- `ConfigSnapshot::get_int(handle)`. **Budget: < 5 ns.**

Print; **do not assert.**

---

## 7. FORBIDDEN

- Adding a file not in the manifest, or touching another `core/` subdirectory.
- **Adding any dependency**, or editing `vcpkg.json`. TOML is P0-08b.
- Any file I/O, `<fstream>`, or `<filesystem>`.
- Changing any signature in the interface contract.
- `std::string`, `std::vector`, `std::map`, `std::unordered_map`, or any
  allocation. The key is a fixed array so a snapshot can be memcpy'd.
- **Matching a key by hash alone.** Compare the text; item 4.
- **An order-dependent `content_hash()`.** Item 7 — this one breaks rule 10.
- Including `version()` in `content_hash()`.
- Coercing between types on read: `get_bool` on an Int is `WrongType`, not
  `true`. A silent coercion here is a limit read as the wrong quantity.
- Returning a default value for a missing key. Return `NotFound`.
- Copying the snapshot in `refresh()` when the version is unchanged.
- Publishing the version before the payload; item 10.
- A concurrent test that runs for a fixed duration and then asserts how much it
  saw. Terminate on the evidence.
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
`sizeof(ConfigSnapshot)`, the measured ns for an unchanged `refresh`, and
confirm that `content_hash()` is order-independent by construction.
