# P1-08a — `instruments/snapshot`: snapshot age, expiry crossing, and the download-failure policy

> Phase 1 · Card 8a of 9 · Status: **DONE** 2026-08-31
> Depends on: P1-01 DONE · P1-06 DONE · P0-02 (`timestamp.hpp`) DONE
> Feeds: P1-02a/b/c, P1-03a, P1-05, P1-08b, P2-04
>
> **Architect's note.** This card is **not blocked**. Like P1-06, it is pure
> logic — it reasons about snapshot ages and expiry boundaries and never opens a
> file or a socket. Synthetic snapshots exercise every path, and the policy
> stays correct unchanged when the parsers and the fetcher land.
>
> It also closes the **`on_download_failure` carried-debt row** that has been
> open since P1-01. `config/altair.toml` sets `snapshot_dir` and
> `keep_snapshots_days` — the right shape — but there has never been a policy
> for *unavailability*, only for *disagreement*. Pre-open is 08:15 and the
> market opens at 09:15: the decision cannot be made at 08:20.

---

## 1. THE DECISION

**The policy is a function of exactly two things: how old the snapshot is in
trading days, and whether an F&O expiry has occurred since it was taken.**

Age alone is not enough, and that is the whole insight. A three-day-old
snapshot mid-cycle is harmless — lot sizes do not change on a Tuesday. A
*one*-day-old snapshot that straddles an expiry is dangerous, because expiry
rollover is precisely when NSE revises lot sizes and when the next cycle's
strikes appear. A pure age threshold gets this exactly backwards on the one
day of the month it matters.

| Situation | Verdict | `stale` | Session |
|---|---|---|---|
| Fetch succeeded | `Fresh` | false | proceeds |
| Failed; snapshot from today or the previous **trading** day, no expiry crossed | `StaleUsable` | true | proceeds, flag raised |
| Failed; older than that, no expiry crossed, within `max_stale_days` | `StaleAging` | true | proceeds, **loud** flag |
| Failed; **an F&O expiry has occurred since the snapshot** | `ExpiryCrossed` | true | source is **withheld from the reconciler** |
| Failed; older than `max_stale_days` | `TooOld` | true | source is **withheld** |
| No snapshot at all | `NoSnapshot` | — | **HALT.** Never start a session with no specs |

**The two "withheld" outcomes need no new blocking machinery, and that is the
point.** A withheld source simply is not `add`ed to the `Reconciler`. P1-06
then sees a contract with no primary and returns `MissingPrimary`, or one with
no broker and returns `NoBroker` — and `verdict_blocks` blocks it. The
mechanism already exists and was built for exactly this. Adding a second
blocking path would give two things that can disagree about whether a symbol
is tradable.

**`NoSnapshot` halts rather than blocking**, because "block every symbol" and
"do not trade today" are the same outcome, and only one of them says so out
loud. Rule 9.

---

## 2. WHY "TRADING DAY", NOT "CALENDAR DAY"

A Monday-morning fetch failure sees a Friday snapshot: three calendar days, one
trading day. A calendar threshold of two would refuse a perfectly good file
every Monday, and a threshold loose enough to survive Monday is four calendar
days — which silently tolerates a genuinely stale file mid-week. Neither is
right. The unit has to be trading days.

Altair has no holiday calendar yet, so this card takes one **as a parameter**
and does not invent it. A parser that guessed Indian market holidays would be
wrong about Diwali Muhurat trading in a way no test here would catch.

---

## 3. FILE MANIFEST

```
CREATE   instruments/snapshot.hpp
CREATE   instruments/tests/test_snapshot.cpp
MODIFY   instruments/CMakeLists.txt
```

`add_test(NAME snapshot COMMAND altair_snapshot_test)`.
**Do not touch** `contract_spec.hpp`, `reconcile.hpp`, the root
`CMakeLists.txt`, or any other directory.

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// Sizing guidance for CALLERS. Nothing here enforces them and nothing here
// stores a calendar — this header only reads the arrays it is handed.
inline constexpr std::size_t kSuggestedHolidayCap = 64;
inline constexpr std::size_t kSuggestedExpiryCap = 512;

enum class SnapshotVerdict : std::uint8_t {
    Fresh,          // the fetch succeeded
    StaleUsable,    // <= 1 trading day old, no expiry crossed
    StaleAging,     // older, still within max_stale_days, no expiry crossed
    ExpiryCrossed,  // an F&O expiry fell between the snapshot and now
    TooOld,         // beyond max_stale_days
    NoSnapshot      // nothing on disk — the session must not start
};

/// True when this verdict means the source is withheld from the Reconciler,
/// so P1-06's existing MissingPrimary / NoBroker verdicts do the blocking.
[[nodiscard]] constexpr bool verdict_withholds(SnapshotVerdict v) noexcept {
    return v == SnapshotVerdict::ExpiryCrossed || v == SnapshotVerdict::TooOld;
}

/// True when the session must not begin at all.
[[nodiscard]] constexpr bool verdict_halts(SnapshotVerdict v) noexcept {
    return v == SnapshotVerdict::NoSnapshot;
}

/// The market calendar, supplied — never invented. Both arrays hold IST
/// midnights and must be sorted ascending.
struct MarketCalendar {
    const Timestamp* holidays;    // full trading holidays
    std::size_t holiday_count;
    const Timestamp* expiries;    // F&O expiry dates, all series
    std::size_t expiry_count;
};

struct SnapshotPolicy {
    std::int32_t max_stale_days;   // UNIT: TRADING days. From config.
};

struct SnapshotState {
    Timestamp taken_at;    // IST midnight of the snapshot's trading date
    bool present;          // false => NoSnapshot
    bool fetch_ok;         // did today's fetch succeed?
};

/// Trading days strictly between `from` and `to`, exclusive of `from` and
/// inclusive of `to`. UNIT: days. Weekends and listed holidays excluded.
/// Negative when `to` precedes `from` — a clock that went backwards is a real
/// event and returning 0 would hide it.
[[nodiscard]] inline std::int32_t
trading_days_between(Timestamp from, Timestamp to,
                     const MarketCalendar& cal) noexcept;

/// True when `to` is strictly after `from` AND a listed expiry falls in the
/// CLOSED interval [from, to]. See requirement 4 — the closed lower bound is
/// a correction, not an oversight.
[[nodiscard]] inline bool
expiry_crossed(Timestamp from, Timestamp to, const MarketCalendar& cal) noexcept;

/// The decision. UNIT: none. `now` comes from the tick or the session clock —
/// never from a wall clock inside this function (rule 7).
[[nodiscard]] inline SnapshotVerdict
judge_snapshot(const SnapshotState& s, Timestamp now,
               const MarketCalendar& cal, const SnapshotPolicy& p) noexcept;

/// What a parser should stamp on every spec it produces from this snapshot.
/// Returns the `stale` flag; the caller sets `snapshot_at` to s.taken_at.
[[nodiscard]] constexpr bool stale_flag_for(SnapshotVerdict v) noexcept {
    return v != SnapshotVerdict::Fresh;
}

} // namespace altair
```

---

## 5. REQUIREMENTS

1. `now` is a parameter. **Nothing in this header reads a wall clock.** Rule 7,
   and it is also the only way the tests can construct a Monday.
2. `trading_days_between` excludes Saturdays and Sundays and every listed
   holiday. Day-of-week is derived in **IST**, not UTC — an IST midnight is
   18:30 UTC the previous day, so a UTC weekday reading is off by one for every
   date. Use P0-02's machinery; do not re-derive it.
3. The holiday and expiry arrays are treated as sorted; the implementation may
   binary-search but must **not** assume sortedness silently — an unsorted
   array is a caller bug and `judge_snapshot` must still be correct, so use a
   linear scan unless sortedness is asserted.
4. **CORRECTED DURING IMPLEMENTATION — see the review record.**
   `expiry_crossed` is true when `to` is strictly after `from` **and** an
   expiry falls in the **closed** interval `[from, to]`. The originally
   specified half-open `(from, to]` was wrong: a master snapshot stamped with
   trading date D is fetched at 08:15 on D, *before* that day's 15:30 expiry,
   so it is a **pre-rollover** file. Excluding `e == from` handed it to D+1's
   session as usable and sized every derivative from the previous cycle's lot
   table. The `to > from` guard keeps the same day safe — this morning's own
   snapshot is fine on the day it was taken, expiry or not.
5. The verdict ladder is evaluated in this order: `NoSnapshot`, `Fresh`,
   `ExpiryCrossed`, `TooOld`, `StaleUsable`, `StaleAging`. **`ExpiryCrossed`
   outranks `TooOld` and both outrank any age-only verdict** — a one-day-old
   snapshot that straddles an expiry must not be reported as `StaleUsable`.
6. `max_stale_days` of 0 means "only today's snapshot is acceptable", not
   "unlimited".
7. A `taken_at` in the future relative to `now` yields `TooOld`, not a negative
   age silently treated as fresh. A snapshot from tomorrow is a broken clock or
   a mislabelled file, and either way it is not trustworthy.
8. No allocation, no exceptions, no floating point.

---

## 6. ACCEPTANCE TESTS

1. **`monday_after_friday_is_one_trading_day`** — a Friday snapshot judged on
   Monday gives `StaleUsable`, not `StaleAging`. Assert the calendar-day gap is
   3 and the trading-day gap is 1, so the test proves the distinction exists
   before proving it is handled.
2. **`holidays_are_excluded`** — inserting a holiday between two dates reduces
   the trading-day count by exactly one; a holiday on a weekend changes nothing.
3. **`ist_weekday_not_utc`** — a Monday IST midnight is Sunday 18:30 UTC.
   Assert the UTC instant explicitly, then assert the function still calls it a
   Monday. Without the first assertion the test cannot fail for the right
   reason.
4. **`expiry_crossing_outranks_age`** — a snapshot **one trading day** old that
   straddles an expiry gives `ExpiryCrossed`, **not** `StaleUsable`. This is
   requirement 5 and the single most important test in the card: it is the case
   where the intuitive answer (young snapshot, therefore fine) is wrong.
5. **`expiry_interval_bounds`** — an expiry **on** the snapshot date **is**
   crossed once we are past that day; an expiry today is crossed; on the day
   itself nothing is crossed; a backwards or zero-length span crosses nothing;
   and an expiry strictly before the snapshot is **not** crossed, so the
   closed bound has not simply made everything true.
6. **`withheld_verdicts_block_through_p1_06`** — take a contract carried only
   by a withheld primary, do not `add` that source to a `Reconciler`, add a
   Kite source, and assert P1-06 returns `MissingPrimary` with
   `verdict_blocks` true. This proves the composition claimed in §1 rather than
   asserting it in a comment.
7. **`no_snapshot_halts`** — `present == false` gives `NoSnapshot`,
   `verdict_halts` is true, and `verdict_withholds` is **false** — halting and
   withholding are different outcomes and must not be conflated.
8. **`future_snapshot_is_too_old`** — `taken_at` after `now` gives `TooOld`.
   Assert the naive signed-difference reading would have called it fresh, so
   the test documents the trap.

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- **Counting calendar days.** Refuses a good file every Monday, or tolerates a
  genuinely stale one mid-week. There is no threshold that does neither.
- **Checking age before expiry crossing.** The one-day-old snapshot that
  straddles a rollover is reported usable, and every derivative in the session
  is sized from the previous cycle's lot table. This is rule 1's failure mode
  arriving through the back door.
- **Deriving the weekday in UTC.** Wrong for every date, and it does not look
  wrong — it looks like an off-by-one nobody can reproduce.
- **Inventing a holiday calendar.** Muhurat trading alone guarantees it is
  wrong, and no test in this card would catch it.

---

## REVIEW RECORD — P1-08a

Reviewed 2026-08-31. MSVC 19.51.36256, `/std:c++latest /W4 /permissive- /O2`.

| Gate | Verdict | Evidence |
|---|---|---|
| 1 compiles clean | PASS | zero warnings, first build. |
| 2 contract honoured | PASS, **card amended** | requirement 4's interval corrected (below); `kMaxHolidays`/`kMaxExpiries` renamed to `kSuggested*Cap`. |
| 3 manifest respected | PASS | only `instruments/snapshot.hpp`, `instruments/tests/test_snapshot.cpp`, `instruments/CMakeLists.txt`. |
| 4 tests pass | PASS | 8 named tests + policy edges, 50 checks, zero failures. |
| 5 no hot-path allocation | PASS | no `ALTAIR_HOT` — pre-open only. No allocation anywhere; the calendar is borrowed, never owned. |
| 6 latency budget | PASS (no budget) | runs once at 08:15. |
| 7 numerical / financial | PASS | no floating point. The whole card is the financial judgement: **age in trading days AND expiry crossing**, because rollover is when lot sizes change. |
| 8 physics | PASS | the IST day boundary is 18:30 UTC the previous day, so weekday is derived in IST via P0-02's `ist_days_between` rather than re-derived. Floor-mod, not truncating `%`, so a pre-1970 day number cannot produce a negative weekday. Every weekday claim in the tests was **verified against an independent implementation**, not asserted from the code under test. |

### The defect review found — in the card, not the code

Requirement 4 originally specified the half-open interval `(from, to]`, and the
implementation matched it. **The specification was wrong.**

A master snapshot stamped with trading date D is fetched at 08:15 on D — before
that day's 15:30 expiry — so it is a **pre-rollover** file. Under `(from, to]`
an expiry on D is `e == from` and therefore *not* crossed, so on D+1 the file
was judged `StaleUsable` and handed to the session. Every derivative would then
be sized from the previous cycle's lot table: **rule 1's exact failure mode,
arriving through the back door of a policy written to prevent it.**

Corrected to a closed lower bound, guarded by `to > from` so the same day stays
safe. We cannot tell from outside whether a given file is pre- or post-
rollover, and rule 9 says ambiguity blocks rather than guesses. Test 5 now
proves both directions, including that the closed bound did not simply make
everything true.

### What did not need building

`verdict_withholds` needs **no blocking machinery of its own**. A withheld
source is simply not `add`ed to the `Reconciler`, and P1-06's existing
`MissingPrimary` / `NoBroker` verdicts block it. Test 6 proves the composition
end to end rather than asserting it in a comment. A second blocking path would
have been a second thing that can disagree with the first about whether a
symbol is tradable.
