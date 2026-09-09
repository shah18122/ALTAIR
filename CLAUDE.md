# CLAUDE.md — Altair

Tick-to-tick, multi-strategy, self-correcting trading engine for NSE & BSE.
C++23 · LibTorch C++ API · **no Python in the runtime**.

---

## Your role in this repo

**You are the architect and reviewer. DeepSeek V4 is the implementer.**

You do not write bulk implementation. You write **task cards**, and you **review
every DeepSeek output against eight gates** before it is committed. 194 cards will
pass through this loop; the interface contract in each card — not anyone's
memory — is what keeps card 90 compiling against card 3.

| Actor | Owns | Never does |
|---|---|---|
| Smit | Decisions, capital, broker accounts, phase go/no-go | — |
| **You (Claude)** | Architecture, task cards, interface contracts, review, phase gates, financial + physics correctness | Write bulk implementation |
| DeepSeek V4 | One task card per prompt, plus its tests | Invent interfaces, add dependencies, touch files outside its manifest |

**Read [`prompts/PROTOCOL.md`](prompts/PROTOCOL.md) before writing or reviewing
anything.** Status board: [`prompts/LEDGER.md`](prompts/LEDGER.md).
Full design: [`ROADMAP.md`](ROADMAP.md).

---

## The loop

```
You write card P<phase>-<nn>  →  Smit pastes to DeepSeek  →  output comes back
        ↑                                                          │
   correction card ←──── fail ──── you run the 8 gates ──── pass ──→ commit + LEDGER
```

### The eight review gates

| # | Gate | Check |
|---|---|---|
| 1 | Compiles clean | zero warnings at `/W4` or `-Wall -Wextra -Wpedantic -Wconversion` |
| 2 | Contract honoured | diff produced headers against the card's contract, byte-level on signatures |
| 3 | Manifest respected | only files listed in the card exist/changed |
| 4 | Tests pass | every named acceptance test present, `ctest` green |
| 5 | No hot-path allocation | no `new`/`malloc`/`vector` growth/`string`/`shared_ptr`/`std::function` in `ALTAIR_HOT` |
| 5b | **Bounds refuse** | every fixed buffer, table and loop cap either refuses, proves it cannot be reached, or counts what it dropped — rule 11 |
| 6 | Latency budget | benchmark vs ROADMAP §11; regression fails |
| 7 | **Numerical / financial** | units · cancellation · boundaries (T→0, IV→0, zero depth, expiry day) · sign conventions · day-count · **which side STT applies to** · premium-vs-notional turnover |
| 8 | **Physics** | dimensional consistency · conservation invariants · no look-ahead · no sampling above Nyquist · error propagation |

**Gate 7 is the one that matters most and is easiest to skip when rushed.**
DeepSeek produces confident, plausible, wrong finance. Never wave it through.

### Reviewing — read the disk, not the summary

DeepSeek reports state relative to *when it started looking*, not when you last
looked. It may claim "already correct" about work an earlier turn did. **Always
`Read` the actual files** and re-derive the numbers yourself before passing a
gate. Verify claims about compiler behaviour; ask for version and error code.

A failed gate produces a **correction card** naming only the defect, never a
from-scratch rewrite. Format is in PROTOCOL.md §7.

---

## Hard rules — these fail review, every time

1. **No lot size, tick size, strike step, or expiry as a literal.** All from the
   point-in-time spec store (`instruments/`). This is the bug that silently
   scaled the predecessor's every P&L number.
2. **No `double` crossing a module boundary carrying money, quantity, or time.**
   Use `core/types` strong units. `Price * Lots` must not compile.
3. **All money is integer paise.** Doubles only in analytics, never the ledger.
4. **No heap allocation inside `ALTAIR_HOT`.** No exceptions on the hot path —
   return `std::expected<T, Error>`.
5. **Every signal is priced net of full cost before it exists.** No strategy sees
   a pre-cost number.
6. **Backtest and live share the same code path.** Replay feed and live feed emit
   the same struct into the same pipeline. If they diverge, the backtest is a lie.
7. **No look-ahead, ever.** Purged CV with embargo, point-in-time fundamentals,
   a replayer that physically cannot expose a future tick. **Strategies read time
   off the tick, never from a wall clock.**
8. **The tighter stop is checked before the original stop.** That ordering bug
   cost ~₹41K in one replay of the predecessor.
9. **Failing loud beats trading wrong.** Ambiguity blocks the affected symbol and
   raises a flag. It never falls back to a guess.
10. **Every live decision is reproducible** from
    `{model_hash, feature_version, config_hash, spec_version, tick_seqno}`.
11. **A fixed bound must refuse what exceeds it.** Every fixed-size buffer,
    lookup table, loop cap and capacity constant has exactly three permitted
    behaviours when the input could exceed it:

    - **Refuse** — return `std::unexpected`, and say which bound was hit.
    - **Prove it cannot be reached**, and state the proof next to the bound.
      `kMaxJumpsPerStep = 64` against a refused `lambda > 12` is a proof:
      P(N > 64) < 1e-25.
    - **Truncate VISIBLY** — count it, carry the count in the result, and put
      it on screen. Only for data loading, where refusing a real row is worse
      than accepting a marked one.

    Silently clamping is banned. **This has been found five times** and every
    one of them returned a plausible number that nothing downstream could
    question:

    | Where | Bound | What it did |
    |---|---|---|
    | `backtest/montecarlo.hpp` | 16 jump arrivals | at λ=20, 78% of steps forced to exactly 16 |
    | `models/markov.hpp` | χ² table ended at df 63 | a 9-state chain — `kMaxStates` — could never reject |
    | `models/recurrent.hpp` | 4,096 rows | GRU trained on 1.9% of 211,000 bars |
    | `models/mlp.hpp` | 8,192 rows | MLP trained on 3.9% of the same |
    | `instruments/kite_dump.hpp` | 23-char underlying | 137 prefixes collide two companies each |

    Three of those are the *same bug written twice with different constants*,
    which is what makes it a rule rather than five fixes.

    **And where a clamp is genuinely unavoidable, it clamps toward the SAFE
    side.** `QLearner::aggression` mapped an out-of-range action to
    `kActions - 1` — the MOST aggressive of the five. A bug that produced a
    bad index traded 3x size.

    The tell is always the same: a returned metric computed over the *same*
    truncated slice, so the one number that would have exposed the truncation
    agreed with it.

---

## Physics discipline

These are enforced, not decorative (ROADMAP §3):

- **Dimensional analysis → the type system.** Timestamps are an affine space;
  durations are the associated vector space. `Timestamp + Timestamp` is as
  meaningless as adding two positions and must not compile.
- **Measurements carry error.** IV, TSC frequency, Kyle's λ are all measured.
  Propagate the error; size on the *lower confidence bound* of edge, never the
  point estimate. A signal whose error bar straddles zero is not a signal.
- **Nyquist sets the architecture.** Order-book imbalance decays in 10–200 ms.
  The predecessor polled at 500 ms and could not, even in principle, see it.
  This is why the engine is event-driven and not a faster poll.
- **Markets are not ergodic.** Walk-forward only; random K-fold is banned in the
  training harness. Report per regime, never only in aggregate.
- **Conservation → runtime invariants.** `Σ(fills) + Σ(costs) + cash_delta == 0`
  exactly, in paise, checked every tick. A breach trips the kill switch.
- **Numerical hygiene.** Stable forms near expiry; two-scale realised variance
  (naive 1-tick RV measures the bid-ask bounce, not volatility); Kahan summation
  on session-long accumulators.

---

## Layout

```
core/          strong types, clock, allocators, lock-free rings, logging, config
instruments/   AUTO-LEARNED contract specs — lot size, tick, expiry, margin
feed/          Kite + XTS decoders, normaliser, failover, replay
book/          L2 book, imbalance, microprice, VPIN, Kyle lambda
analytics/     greeks, IV/SVI, rolling stats, derivatives, India VIX
features/      versioned, horizon-banded feature registry
models/        LibTorch training, ONNX serving, Monte Carlo, DCF, aggregator
strategies/    arbitrage, quant momentum, 10-min forecast, sector hedge, vol
risk/          sizing, limits, portfolio greeks, COST CALCULATOR
oms/           THE TRADE HANDLER — router, broker adapters, state machine,
               reconciliation. Nothing else places or amends an order.
flagging/      per-model scorecards, drift detection, auto-correction
backtest/      tick replayer, walk-forward, purged CV
server/        BACKEND ONLY — the binary delta-frame protocol, auth, session.
               Renders nothing and knows no pixels. Serves a REMOTE client;
               the desktop UI does not go through it.
desktop/       THE UI — Qt 6 Widgets, C++23, IN THE SAME PROCESS as the
               engine. Grid, charts, depth ladder, panels. Links read-side
               engine headers only; holds no oms/ handle and has no
               order-placing vocabulary. See "The in-process decision".
client/        RETIRED — the TypeScript SPA from Phase 11. Kept as a record
               of its findings and their tests; not built, not shipped.
               See client/README.md.
app/           main() — the `altair` binary
dataset/       TRAINING AND RESEARCH DATA, partitioned by segment then symbol
research/      papers/inbox/ — PDFs get dropped here
config/        altair.toml, charges.toml (effective-dated), strategies/*.toml
prompts/       PROTOCOL.md, LEDGER.md, task cards
ops/           RUNBOOKS — Linux deployment, disaster recovery, go-live.
               Documents only; nothing here is built, linked or tested, and
               no card manifest may list a file in it. Every claim states
               whether it has been verified and on what.
RXT_trade*/    predecessor Python tree — REFERENCE ONLY, not part of the build
Quants/        REFERENCE ONLY — the QUANTLAB Python tree. Its MODELS were
               reimplemented here in Phases 13–19 (Smit, 2026-09-06); its
               FINDINGS are the valuable import and are recorded in
               prompts/PHASE13_PLAN.md §2. Not built, not linked, and no
               card manifest may list a file in it. Kept because four of its
               results are negative ones this project would otherwise pay to
               rediscover.
```

### One component, one directory — this is a hard rule

A directory is a **deployment and blast-radius boundary**, not a filing
convenience.

- **`oms/` is the only thing that can place an order.** If order-placing code
  appears anywhere else, that is a review failure, not a refactor opportunity.
  This rule is unconditional and survives everything below.
- **`server/` and a remote client never share a header.** They share a wire
  protocol and nothing more, kept honest by conformance vectors both sides
  test against (`server/tests/vectors/`). Shared test vectors, not shared
  code — if the implementations drift, the vectors fail rather than the
  dashboard quietly showing a wrong number.
- A card's manifest **never spans two of these directories.** If a change
  needs both, it is two cards with an explicit interface between them.

### The in-process decision — Smit, 2026-09-03

`desktop/` links the engine and runs in the same process. **This was decided
against the recommendation above, deliberately, and the tradeoff is recorded
here rather than left to be rediscovered.**

What was given up: **blast radius.** A paint bug, a bad cast in a chart, an
out-of-range index in the grid — any of them now takes down the process that
is holding live positions. Out of process, the engine would have survived and
the window would have died alone. There is no way to mitigate this from inside
one address space, and it is the reason the rule was written the other way.

What was gained: no serialisation, no IPC, no second copy of every view
struct, and one language and one build for the whole program. For a
single-operator desktop tool that is a real and defensible saving.

**What is preserved anyway, and is not optional:**

1. `desktop/` includes **read-side headers only** — `core/types`, `book/`,
   `analytics/`, `risk/` (for reading exposure), `flagging/`. It does not
   include `oms/` or `broker/`, and CMake does not link them into the UI
   target.
2. The UI holds **no handle that can place an order**. Its only mutating
   channel is a kill-switch request, which still goes through the same
   confirmation as P11-14 and is still executed by `oms/`, never by the UI.
3. Because the compiler will not stop a determined `#include`, **gate 3
   (manifest) explicitly checks the UI target's link libraries.** A UI target
   that links `altair_oms` fails review.

The safety property that matters — *the UI cannot trade* — is therefore kept
by construction. The one that was traded away — *the UI cannot crash the
engine* — is gone, and this paragraph exists so nobody later reads the code
and assumes it was an oversight.

### `dataset/` layout

Training data is **directory-partitioned**, `dataset/<segment>/<symbol>/`:

```
dataset/
  spot/          # cash / underlying
    nifty/       # e.g. dataset/spot/nifty/
    banknifty/
    reliance/
  fut/
    nifty/
  opt/
    nifty/
```

The partition is the point: a model trained on NIFTY spot reads exactly one
directory, so its training set is defined by a path rather than by a filter
someone has to get right. It also makes walk-forward folds and per-symbol
retraining trivially separable, and makes it impossible to leak another
symbol's data into a fold by accident.

Segment names on disk are `spot` (`Segment::Cash`), `fut`, `opt`, `cur`, `com`.
Symbols are lower-case. `dataset/` is gitignored — it is large and regenerable.

## Build

```powershell
.\build.bat                     # configure + build + test, preset `default`
.\build.bat debug               # any other preset by name
```

`build.bat` is a thin wrapper, not a second build system. It exists because on
this box neither `cmake` nor `ninja` is on `PATH` — both ship inside VS Build
Tools — and because `vcvars64.bat` cannot export into PowerShell: it runs in a
child process and its environment dies with it. The wrapper calls `vcvars` in
the *same* cmd process, prepends the bundled `cmake`/`ninja`, then runs:

```powershell
cmake --preset default          # no vcpkg needed until P0-05
cmake --build --preset default
ctest --preset default
```

Those three are still the real commands, and work directly from a Developer
Command Prompt with cmake and ninja on `PATH`.

Presets: `default` · `vcpkg` · `debug` · `asan` · `tsan` · `prod`.

### Building `desktop/` (the Qt UI)

Qt 6.8.3 LTS, MSVC 2022 x64, installed at `D:\Qt` — off C: deliberately, which
had 7.8 GB free against a 230 GB disk. Fetched with `aqtinstall`, not the Qt
online installer, so the version is reproducible from a command line:

```powershell
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O D:\Qt
```

CMake finds it through `ALTAIR_QT_ROOT` (see `CMakePresets.json`). The UI is
part of the normal build; there is no separate frontend toolchain and no
`npm`.

Qt is **LGPLv3** here. Dynamic linking only — do not static-link Qt into a
distributed binary without reading the licence.

### `client/` is retired

The TypeScript SPA is kept as a record and is **not built**. Do not add it to
a build, a CI step, or a card manifest. `client/README.md` says what survived
into `desktop/` and what was language-specific and died with it.

### Seeing it run

```powershell
.\build\default\app\altair.exe --instruments    # Phase 1, end to end
.\build\default\app\altair.exe --selftest       # Phase 0 wiring
.\build\default\app\altair.exe --help
```

`--instruments` walks the whole instrument-master pipeline: the
download-failure policy, the Kite parser, three-way reconciliation, and the
spec store — with a built-in sample so it runs against no files. Pass a real
Kite `instruments.csv` as an argument to run it at full scale.

---

## Card sizing

Split the card if: any file would exceed ~400 lines · more than 4 files in the
manifest · more than 12 numbered requirements · more than 8 acceptance tests.

**If a card would require DeepSeek to make a design decision — stop.** You make
the decision and put it in the interface contract.

---

## Carried debt

Tracked at the top of [`prompts/LEDGER.md`](prompts/LEDGER.md). Currently:
`apply_bps` uses `long double` (64-bit on MSVC) — exact for single-trade
magnitudes, must be revisited with scaled-integer arithmetic in P3-09 where
session-accumulated turnover is involved.

---

## Reality checks — do not let these get lost

- **Retail broker APIs cap you at 10–50 ms round trips.** Pure latency-arbitrage
  is unavailable. Altair competes on model quality, breadth, and cost discipline.
  The µs architecture buys headroom for a future DMA/FIX line and the ability to
  evaluate every model on every tick.
- **Most textbook arbitrage in NSE/BSE is already gone** after STT, exchange
  charges, GST, stamp duty, and impact. Phase 5's most valuable output may be
  proving that.
- **STT rose on 2026-04-01** (futures 0.02→0.05%, options 0.10→0.15% sell-side
  premium). Every pre-April backtest is optimistic until re-run.
- **10-minute directional accuracy tops out around 52–55%.** Anything claiming
  70% is overfit.
- **Quarterly retraining degrades as easily as it improves.** Shadow → canary →
  auto-rollback is what stops it becoming a slow-motion self-inflicted loss.
