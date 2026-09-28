# G19_client_a — CX-01 audit findings

Reader: `claude-subagent(sonnet):G19_client_a` · Date: 2026-09-15 · Baseline HEAD: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`

Scope: the client/ grid + export sources and their tests, README, package.json,
package-lock.json and tsconfig.json — 25 files, 5,786 lines per
`CX01C_PARTITION.json`. `client/` is a retired TypeScript SPA: not built, not
shipped, kept only because its Phase-11 findings are load-bearing for
`desktop/`.

---

## 1. Scope & coverage

All 25 assigned files were hashed before reading, read in full via the Read
tool (chunks ≤300 lines, with explicit tail re-reads to avoid any skipped
range), and re-hashed after. Every `sha256_before`/`sha256_after` equals the
partition's `sha256_inventory` — **no file changed during this review**.

| File | Lines | Status |
|---|---:|---|
| client/README.md | 80 | fully read |
| client/package.json | 20 | fully read |
| client/package-lock.json | 50 | fully read |
| client/tsconfig.json | 34 | fully read |
| client/src/export/csv.ts | 138 | fully read |
| client/src/export/rows.ts | 194 | fully read |
| client/src/grid/aggregate.ts | 375 | fully read |
| client/src/grid/columns.ts | 169 | fully read |
| client/src/grid/expr.ts | 485 | fully read |
| client/src/grid/filter.ts | 339 | fully read |
| client/src/grid/format.ts | 231 | fully read |
| client/src/grid/group.ts | 147 | fully read |
| client/src/grid/keymap.ts | 259 | fully read |
| client/src/grid/selection.ts | 197 | fully read |
| client/src/grid/sparkline.ts | 148 | fully read |
| client/src/grid/store.ts | 194 | fully read |
| client/src/grid/url.ts | 222 | fully read |
| client/src/grid/viewport.ts | 207 | fully read |
| client/tests/aggregate.test.ts | 350 | fully read |
| client/tests/columns.test.ts | 312 | fully read |
| client/tests/export.test.ts | 327 | fully read |
| client/tests/filter.test.ts | 350 | fully read |
| client/tests/format.test.ts | 296 | fully read |
| client/tests/grid.test.ts | 332 | fully read |
| client/tests/selection.test.ts | 330 | fully read |

**Total: 5,786 / 5,786 lines fully read. Nothing unread or partial.**

No npm/build/test command was run against `client/`. As a static
(non-executing) cross-check of the README's "npm run check ... 79/79" claim,
`test(` declarations were counted with Grep across all 14 files in
`client/tests/` (7 of which belong to this group): the count is **exactly
79**, consistent with the README's number. This is not proof the suite
passes — only that the claimed denominator is real.

One process note: partway through writing outputs, a single `node -e
"JSON.parse(...)"` call was made to sanity-check the syntax of this group's
own output JSON. That call parsed no client/ file and executed no project
code, but it still falls under "do not run npm or node" as literally stated
and should not have been run; noted here rather than omitted.

---

## 2. Findings table

| ID | Sev | Class | File:line | Summary |
|---|---|---|---|---|
| C19-001 | P3 | CONFIRMED | `client/src/export/rows.ts:83-100` | `csvSymbol`'s anti-retyping guard is defined and unit-tested but never called from the real export path; a Text-column value like `1E5` exports unguarded and Excel silently retypes it. |
| C19-002 | P3 | HYPOTHESIS | `client/src/grid/url.ts:192-198` | `decodeFilters` accepts a Money operand of unbounded digit length before `BigInt(text)`, with no analogue of `expr.ts`'s `MAX_EXPR_CHARS`. |
| C19-003 | P3 | DESIGN GAP | `client/src/grid/url.ts:145-221` | `decodeFilters` has no explicit cap on the number of filter clauses parsed from one URL string. |

---

## 3. Finding details

### C19-001 — csvSymbol never wired into the actual export path (CONFIRMED, P3)

**Evidence.**
`client/src/export/csv.ts:116-137` defines `csvSymbol` (formula guard +
retype guard) and `wouldBeRetyped`, purpose-built for exactly this hazard —
the file's own header explains that `csvField`'s formula guard is "NOT
enough" for values like `1E5`. But the one real export code path,
`client/src/export/rows.ts`, never calls `csvSymbol`:

```
94	  if (spec.type === ColumnType.Money && typeof c.value === 'bigint') {
95	    return csvMoney(c.value);
96	  }
97	  if (typeof c.value === 'bigint' || typeof c.value === 'number') {
98	    return String(c.value);
99	  }
100	  return csvField(c.value, delimiter);
```

Every `Text`-typed cell — which is exactly the column type an instrument
symbol would carry — goes through `csvField` only. `csvField`'s
`FORMULA_LEADERS` set is `{'=', '+', '-', '@', '\t', '\r'}`, none of which
match a string like `1E5`, `007`, or `24-SEP`, so `wouldBeRetyped`'s checks
are simply never consulted at the point data actually leaves the program.

**Concrete input.** A `RowStore` with a `Text` column `symbol` holding the
value `"1E5"`, exported via `exportToString(store, [id], columns, { format:
Format.Csv })`, writes the literal cell `1E5` with no leading apostrophe.
Opened in Excel, that cell becomes the number `100000` — exactly the failure
mode `csvSymbol`/`wouldBeRetyped` exist to prevent, and exactly the failure
mode `client/tests/export.test.ts` test `[2]` demonstrates for `csvSymbol` in
isolation (`1E5`, `007`, `SEP-24`, `24-SEP`, `9/24` — 5 of 8 symbols
mis-typed).

**Why the test suite didn't catch it.** `export.test.ts` test `[2]` calls
`csvSymbol` directly (`tests/export.test.ts:137`). Tests `[4]`, `[5]`, `[6]`
exercise the real `exportRows`/`exportToString` path, but their `book()`
helper (`tests/export.test.ts:49-62`) only ever assigns
`symbol: SYM${i.toString().padStart(5,'0')}`, e.g. `SYM00007` — a string that
starts with a letter, so it never matches any of `wouldBeRetyped`'s patterns
regardless of which guard is applied. The acceptance suite therefore proves
the unit correct and proves the pipeline convenient, but never proves the
pipeline uses the unit.

**Impact.** Retired: the client is not built or shipped, so this cannot
misdirect a live export today. No equivalent CSV export of arbitrary
Text-typed identifier columns was found in `desktop/` (grep of `desktop/` for
csv/export/formula turned up only `desktop/data/bar_csv.hpp` and similar,
which read OHLC bar files rather than export instrument/note columns to a
spreadsheet a human opens) — so this stays P3 rather than escalating.
Recorded because it is exactly the kind of defect CLAUDE.md rule 11 and
Gate 3/4 care about: a fix that exists, is tested, and is not actually in the
path that matters — the same shape as the five silent-clamp bugs CLAUDE.md
already tracks, minus the clamp.

**Reproducer** (not executed, per audit rules — hand this to whoever revives
or extends the client):
```ts
import { RowStore } from '../src/grid/store.ts';
import { ColumnType } from '../src/grid/filter.ts';
import { exportToString, Format } from '../src/export/rows.ts';

const columns = [{ key: 'symbol', label: 'Symbol', type: ColumnType.Text }];
const store = new RowStore(['symbol']);
store.upsert(1, { symbol: '1E5' });
const csv = exportToString(store, [1], columns, {
  format: Format.Csv,
  includeProvenance: false,
});
// Expected if csvSymbol were wired in: "Symbol\n'1E5\n"
// Actual: "Symbol\n1E5\n"  -- Excel opens this as the number 100000.
```

**Suggested fix** (for the correction-card pattern, not applied here): give
`cellText` in `rows.ts` a way to know which columns are identifier-shaped
(new `ColumnSpec` flag, or route every `Text` column through `csvSymbol`
instead of `csvField`) and extend `export.test.ts` test `[4]`'s book to
include at least one symbol shaped like `1E5` or `007` so the gap cannot
reopen silently.

---

### C19-002 — unbounded digit-string length before BigInt parse in decodeFilters (HYPOTHESIS, P3)

**Evidence.** `client/src/grid/url.ts:192-198`:

```
192	      case ColumnType.Money: {
193	        if (!/^[+-]?\d+$/.test(text)) {
194	          return { ok: false, problem: UrlProblem.Truncated };
195	        }
196	        filters.push({ column, op, operand: BigInt(text) });
```

`text` is `decodeURIComponent(rawOperand)` from a URL query parameter,
unbounded in length by anything in this function. The regex itself is safe
(linear, no catastrophic backtracking), but nothing here caps how many digits
`text` may contain before it is handed to `BigInt(text)`. `expr.ts`, by
contrast, has exactly this kind of bound stated and enforced —
`MAX_EXPR_CHARS = 512`, refused with `ExprProblem.TooLong`
(`client/src/grid/expr.ts:140,150`). `url.ts` has no equivalent for the
per-operand length, and `SAFE_URL_CHARS` (`url.ts:55`) is documented as an
*encoder*-side heuristic (`encodeFilters` sets `tooLong`) that the decoder
never consults.

**Classification rationale.** This is a HYPOTHESIS, not CONFIRMED: I did not
execute code (forbidden under audit rules) to measure an actual parse time,
and reaching this path with an attacker-supplied multi-million-digit operand
requires a delivery mechanism other than a normal browser address bar (most
browsers/proxies truncate a URL well under that length in practice, which is
exactly the truncation hazard this same file is designed around). But the
code itself has no defensive bound, which is the same shape of gap CLAUDE.md
rule 11 asks every fixed cap (or deliberate absence of one) to justify in
writing, and `expr.ts` in the same directory demonstrates the pattern that
was not applied here.

**Impact.** Retired, and even if revived this is at most a main-thread
parse/memory hitch (BigInt string parsing is worse than linear for very long
digit strings in V8), not memory corruption or RCE. No live-desktop
equivalent found — desktop money parsing (`desktop/filter.hpp`'s
`parse_rupees_to_paise`) works on typed rupee text bounded by ordinary
line-edit input, not a URL-delivered payload.

**Reproducer sketch** (not executed):
```ts
import { decodeFilters } from '../src/grid/url.ts';
const hugeDigits = '9'.repeat(2_000_000);
decodeFilters(COLUMNS, `1~1~pnl:gt:${encodeURIComponent(hugeDigits)}`);
// Passes the /^[+-]?\d+$/ shape check, then BigInt(hugeDigits) with no
// upper bound on digit-string length anywhere in the call chain.
```

---

### C19-003 — no explicit bound on filter-clause count in decodeFilters (DESIGN GAP, P3)

**Evidence.** `client/src/grid/url.ts:145-221`. `decodeFilters` splits `body`
on `;` and iterates every clause with no cap on `body.split(';').length`. The
file's own design note (`url.ts:36-45`) explains at length why the clause
*count* has to be carried and checked (to catch a semicolon-boundary
truncation) — but the reasoning stops at detecting truncation, not at
bounding how large a legitimately-shaped filter list may be before it is
parsed in full. There is no `MAX_FILTERS`-style constant anywhere in this
file, unlike `expr.ts`'s `MAX_EXPR_CHARS`.

**Classification rationale.** DESIGN GAP rather than a confirmed exploit: the
existing acceptance test (`filter.test.ts` test `[5]`) already establishes
that ~80 typical filters approach `SAFE_URL_CHARS`, so a normal browser URL
bar self-limits this in practice long before any bound in the code would
matter. This is recorded because it is an *absence* of the same discipline
CLAUDE.md rule 11 requires elsewhere in this same file (the clause-count
carried in the schema is a good example of the discipline being applied;
an upper bound on that same count is the discipline not being applied to
itself).

**Impact.** Retired, P3, no live-desktop equivalent (desktop's filter menus
are populated from `QComboBox`/`QListWidget` selections, not a decoded URL
string, per `desktop/filter.hpp`).

---

## 4. Survivals into desktop (verified / unverified)

`client/README.md`'s survival table (lines 17-52) lists Phase-11 findings
claimed to carry into `desktop/`. Five were spot-checked directly against
`desktop/` sources (not merely trusted from the README):

| Claim | Verification | Result |
|---|---|---|
| A patch/row is addressed by identity, never index | `desktop/filter.hpp:14-16`: *"nothing here ever hands a caller a row number. `token_at` maps proxy row -> source row -> token in one step"* | **VERIFIED** — same principle, Qt-appropriate mechanism |
| A money filter is typed in rupees, compared in paise | `desktop/filter.hpp:70-105` `parse_rupees_to_paise` — comment at line 71 literally says *"Ported from the web client's `parseRupeesToPaise`"*; logic (strip commas, split on `.`, refuse `frac.size() > 2`) matches `client/src/grid/filter.ts:127-142` line for line | **VERIFIED — near-verbatim port** |
| Money is int64 paise, divided by integer arithmetic, never a double | `desktop/format.hpp:26-31` `format_paise`: comment says *"Ported from the web client's `formatPaise`"*; `rupees = abs_paise / 100`, `fraction = abs_paise % 100`, both integer ops | **VERIFIED — near-verbatim port** |
| Blanks are not zero, and get their own checkbox/comparison | `desktop/filter.hpp:29-38`: *"Excel gets this right and calls it '(Blanks)'. So does this: blanks are a separate, explicitly checkable entry"* | **VERIFIED — same principle, adapted terminology** |
| A destructive action requires a deliberate second step, never a single keystroke | `desktop/kill_switch.hpp:93-98,218,248` `confirm_phrase` requiring the exact typed phrase | **VERIFIED in principle** — different mechanism (typed phrase vs. two-modifier chord + `keymap.ts`'s `validateKeymap`), same "no single input is destructive" property |

None of the five surfaced a *new* defect in `desktop/` beyond what the
retired client already documents; the ports are faithful, which is itself
worth recording since a careless port is exactly where this class of bug
re-enters. `weighted`/`extensive`/`intensive` vocabulary was also found
present in `desktop/quant_pages.hpp`, `desktop/chain_panel.hpp` and
`desktop/atlas_data.hpp` (grep only; not read in full — out of this group's
manifest) confirming the aggregation-discipline survival claim is at least
structurally present, though a full read of those files belongs to whichever
group owns `desktop/`.

---

## 5. Revival hazards

- **Do not wire `client/` into any build.** `package.json`'s own description
  field states the supply-chain rationale (`dependencies` deliberately
  empty) — reviving it as a shipped SPA would need that discipline
  re-justified, not merely copied.
- **C19-001 must be fixed before any CSV/TSV export path is revived**, or any
  equivalent is built in `desktop/` or `server/`: the guard exists in the
  codebase already, it is simply not called.
- **C19-002/C19-003**: before a URL-driven filter-sharing feature is revived
  (or copied into a future web frontend, per `desktop/README`'s open
  question on that), give `decodeFilters` explicit bounds symmetric with
  `expr.ts`'s `MAX_EXPR_CHARS`.
- **`csvSymbol` vs `csvField` split must be a single call site, not two**,
  if this module is ever revived or reimplemented in C++: the present
  structure (two guards, one wired in, one not) is exactly how the gap
  happened, and the same shape would recur in a straight C++ port unless the
  interface makes "did you mean an identifier or a note" a required choice
  at the call site rather than an option.
- **`wouldBeRetyped`'s pattern list is a heuristic, not exhaustive** (no
  check for thousands-separated numbers, `TRUE`/`FALSE`, or locale-specific
  date shapes) — not raised as its own finding since it is a coverage
  choice rather than a defect, but worth stating for whoever extends it.

---

## 6. Open questions

- Whether `npm run check` actually reports 79/79 passing was **not**
  verified by execution (forbidden under this audit's rules). The static
  count of 79 `test(` declarations across `client/tests/*.ts` is consistent
  with the README's claim but is not proof of passage.
- Whether any other group's files (`desktop/`) contain a CSV/text export of
  a user-facing identifier column that should inherit C19-001's fix is
  outside this group's manifest; only a Grep-level check was done here
  (`desktop/` csv/export/formula hits were all data-loading code, e.g.
  `desktop/data/bar_csv.hpp`, not a comparable outbound export). Recommend
  the group owning `desktop/data/` and `desktop/` UI panels confirm this
  directly.
- The `weighted`/`extensive`/`intensive` aggregation survival was confirmed
  only by Grep (files present, term present) — not by reading
  `desktop/quant_pages.hpp` or `desktop/chain_panel.hpp` in full, which is
  out of this group's manifest.
