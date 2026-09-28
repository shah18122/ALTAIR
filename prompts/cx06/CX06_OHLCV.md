# CX06-D1 / D2 — read-only intraday OHLCV reconciliation

Frozen before implementation, 2026-09-16. The user's explicit implementation
request supersedes the historical architect/DeepSeek role split; safety gates
and cold-path separation remain in force. This does not alter Claude's loaders,
writers, engine, or historical files.

## Context and manifest

D1: `desktop/data/bar_consistency.hpp` and
`desktop/tests/test_bar_consistency.cpp` (core comparison and tests).
D2: `desktop/data/bar_consistency_io.hpp` and
`desktop/data/bar_consistency.cpp` (strict CSV adapter and read-only CLI).
This document records the contract and evidence; CMake registration is a separate
root-agent integration card. Each source file stays below approximately 400 lines.

## Interface contract

Namespace `altair::ui::barcheck`. `Bar` holds a UTC epoch-second start,
four integer-paise OHLC fields, and optional nonnegative integer volume.
`Series` is `std::map<qint64, Bar>`, ordered by UTC start.

```cpp
bool parse_price(const QByteArray& text, bool allow_rounding,
                 qint64& paise, bool& rounded);
bool parse_volume(const QByteArray& text, qint64& units);
void compare(const Series& base, const Series& target,
             int base_minutes, int target_minutes, Report& report);
Series load(const QString& path, int minutes, bool round_subpaise,
            bool zero_volume_absent, Report& report);
```

`Report` counts every diagnostic by code, stores at most 200 detailed diagnostics
by default, and reports the omitted-detail count. It tracks matched windows,
price comparisons, comparable/unknown volume windows, parsed rows and explicitly
rounded fields. `ok()` means no diagnostic and at least one matched window;
it never certifies source/provider/contract correctness. JSON additionally sets
`ohlcv_fully_verified` false whenever either issues or unverified-volume windows
exist; a known-fields pass is not mislabeled complete OHLCV verification.

## Behavioural spec

1. Only explicitly timestamped intraday bar-open CSVs are supported, with header
   `time,open,high,low,close,volume` and optionally trailing `oi`. Quoted fields,
   exponent prices, nonfinite values, negative prices/volumes, missing OHLC,
   invalid ranges, duplicates and ambiguous schemas fail visibly. Other columns
   are refused, not guessed. OI is outside the OHLCV contract and is not compared.
2. Timestamps must be second precision `YYYY-MM-DDTHH:mm:ss+05:30`; they are
   interpreted as interval opens, never closes. Only 09:15–15:30 IST is supported.
   Daily bars, auctions, special sessions and other timezones are not inferred.
3. Positive money is parsed in checked integer arithmetic. More than two decimal
   places with nonzero discarded digits fail by default. `--round-subpaise`
   explicitly permits half-up rounding and counts every changed price field.
4. Base and target durations are positive integer minutes, target > base,
   target <= 375, target divisible by base. Every timestamp must align to its
   own duration measured from 09:15, not UTC midnight.
5. On every date present in either input, check the entire regular session.
   Missing whole dates in both inputs cannot be discovered without an exchange
   calendar and are explicitly outside scope. Missing dates on just one side,
   internal gaps and a partial latest session cannot pass.
6. Each target is built from exactly the expected contiguous base timestamps:
   open first, high max, low min, close last; known volumes sum with checked
   addition. A session's final short bucket is explicitly truncated at 15:30,
   e.g. 60-minute bars end with a 15-minute bucket beginning 15:15.
7. Empty volume means unknown, not zero. Zero is known unless explicitly requested
   with `--zero-volume-absent`. Unknown-versus-known volume is a diagnostic;
   unknown on both sides is recorded as unverified volume, not asserted equal.
8. Inputs can be individual CSV files or nonrecursive directories of CSV files.
   Input readers never write or repair any source. Safety caps refuse explicitly:
   10,000 files, 2,000,000 data rows per input, 4,096-byte physical lines,
   20,000 session dates. Every cap is reported, with omitted details counted.
9. CLI options: `--base`, `--target`, `--base-minutes`, `--target-minutes`,
   `--round-subpaise`, `--zero-volume-absent`. Output is JSON on stdout, no output
   path argument. Exit 0 means scoped consistency, 1 means invalid/incomplete/
   mismatch, 2 means usage error. Money, volume and timestamps in diagnostics
   are strings where needed to avoid JSON number precision loss.

## Constraints / forbidden

QtCore and C++23 only, no new dependency, network, broker, OMS or runtime engine
integration. This is an allocating cold-path audit utility, never `ALTAIR_HOT`.
No source overwrite, imputation, silent rounding, silent truncation, automatic
contract/provider equivalence, or claim that the project is safe to trade.

## Acceptance tests

- `test_five_minute_conservation`: 375 one-minute bars and 75 five-minute bars;
  first aggregate OHLC = 10000/10414/9990/10402 paise and volume = 15;
  changing high by one paise fails.
- `test_session_anchor_and_short_tail`: 375 one-minute bars reconcile with
  seven hourly bars anchored 09:15; the 15:15 final interval contains 15 rows.
- `test_gap_and_duplicate_refusal`: absent base minute, absent target bucket,
  duplicate CSV timestamps, and misaligned timestamps all fail explicitly.
- `test_volume_semantics_and_overflow`: empty/empty is unverified, known/unknown
  fails, true zero/zero passes, and int64 volume sum overflow is refused.
- `test_numeric_limits`: largest signed 64-bit paise accepted exactly; a one-paise
  larger value, huge integer, exponent, negative and malformed values refused;
  explicit subpaise rounding counted, strict subpaise rejected.
- `test_reader_schema_and_bounds`: invalid timestamp/header/range, long line,
  malformed volume and nonexistent input cannot pass; diagnostics cap counts
  all omitted details and preserves aggregate error counts.
- `test_directory_and_missing_sessions`: directories merge without dropping
  duplicates; a missing session on one side fails and source bytes are unchanged.
- `test_cli_contract`: invalid duration, empty input, and incomplete end-of-day
  cannot pass; report states regular-session/provider/calendar limitations.

## Rules

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

## Verification

Implementation complete in the four listed C++ sources. Root-agent build/test
and read-only audit results are recorded in `VERIFICATION.md` and
`OHLCV_RESULTS.json`. Targets: `altair_bar_consistency` from
`desktop/data/bar_consistency.cpp`, and `test_bar_consistency` from
`desktop/tests/test_bar_consistency.cpp`, both linked **only `Qt6::Core`**.
The test locates the checker beside itself, so the test target must depend on the
CLI target. No Q_OBJECT/moc, network, widget, engine or broker dependency.

Early read-only observations (superseded by timestamped `OHLCV_RESULTS.json`
where the data workflow has subsequently updated September files):

- `dataset/spot/nifty/1m/2026-09.csv` ends 2026-09-15 10:25 IST;
  `5m/2026-09.csv` ends 10:20 IST. This latest session is incomplete, so the
  full-session audit must not declare the September file fully consistent.
- `dataset/fut/nifty/5m/2026-09.csv` includes 15:30 and 15:35 timestamps on
  September 8, while the hourly file's final open is 15:15. Such data cannot be
  silently reconciled against a 15:30 close; investigate session/source semantics.
- Intraday spot CSVs have six OHLCV columns; futures additionally carry `oi`.
  Current samples use explicitly timestamped opens and decimal rupees.

Suggested commands (run from the repository root after building):

```powershell
.\build\codex-cx06\desktop\altair_bar_consistency.exe --base dataset/spot/nifty/1m/2026-08.csv --target dataset/spot/nifty/5m/2026-08.csv --base-minutes 1 --target-minutes 5
.\build\codex-cx06\desktop\altair_bar_consistency.exe --base dataset/spot/nifty/1m/2026-09.csv --target dataset/spot/nifty/5m/2026-09.csv --base-minutes 1 --target-minutes 5
.\build\codex-cx06\desktop\altair_bar_consistency.exe --base dataset/fut/nifty/5m/2026-08.csv --target dataset/fut/nifty/60m/2026-08.csv --base-minutes 5 --target-minutes 60
```

Do not interpret these instructions as a completed native phase, a financial
truth certification, or permission to rewrite historical bars.
