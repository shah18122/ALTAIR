# Altair

Tick-to-tick, multi-strategy, self-correcting trading engine for NSE & BSE.
C++23 · LibTorch C++ API · no Python in the runtime.

**Read [`ROADMAP.md`](ROADMAP.md) first.** It is the single source of truth for
architecture, phases, and the rules everything is built under.

---

## Who does what

| Actor | Role |
|---|---|
| **Smit** | Decisions, capital, broker accounts, phase go/no-go |
| **Claude** | Architecture, task cards, interface contracts, review of every output |
| **DeepSeek V4** | Implements exactly one task card per prompt, plus its tests |

The workflow is defined in [`prompts/PROTOCOL.md`](prompts/PROTOCOL.md).
Progress is tracked in [`prompts/LEDGER.md`](prompts/LEDGER.md).

---

## Build

### Prerequisites

| Tool | Version | Needed from |
|---|---|---|
| CMake | ≥ 3.28 | now |
| Ninja | any | now |
| Compiler | MSVC 19.4x / GCC 14 / Clang 18 | now (C++23) |
| vcpkg | current, `VCPKG_ROOT` set | card P0-03 |

**Cards P0-01 and P0-02 need nothing but the standard library**, so the tree
builds before vcpkg is set up at all.

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

From P0-03, switch to the vcpkg preset — first pin the baseline:

```powershell
cd $env:VCPKG_ROOT; git rev-parse HEAD   # paste into vcpkg.json "builtin-baseline"
cd C:\PycharmProjects\altair
cmake --preset vcpkg
cmake --build --preset vcpkg
```

Presets: `default` · `vcpkg` · `debug` · `asan` · `tsan` · `prod`.

### Useful options

```
-DALTAIR_BUILD_TESTS=ON/OFF        unit tests           (default ON)
-DALTAIR_BUILD_BENCH=ON/OFF        latency benchmarks   (default ON)
-DALTAIR_ENABLE_ASAN=ON            address + UB sanitizer
-DALTAIR_ENABLE_TSAN=ON            thread sanitizer
-DALTAIR_ENABLE_TORCH=ON           build the ML tier    (Phase 8+)
-DALTAIR_STRICT_INVARIANTS=ON/OFF  conservation checks every tick (default ON)
```

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
oms/           router, broker adapters, state machine, reconciliation
flagging/      per-model scorecards, drift detection, auto-correction
backtest/      tick replayer, walk-forward, purged CV
ui/            uWebSockets server, Excel-grade grid, WebGL charts
research/      papers/inbox/ — drop PDFs here
config/        altair.toml, charges.toml, strategies/*.toml
prompts/       PROTOCOL.md, LEDGER.md, and the task cards
data/          tick store, dated instrument snapshots, fundamentals (gitignored)
```

---

## Non-negotiables

These fail code review, every time:

1. No lot size, tick size, strike step, or expiry as a literal — all from the
   point-in-time spec store.
2. No `double` crossing a module boundary carrying money, quantity, or time —
   use the strong types in `core/types`.
3. No heap allocation inside a function marked `ALTAIR_HOT`.
4. No exceptions on the hot path — return `std::expected<T, Error>`.
5. Every signal is priced net of full cost before it exists.
6. Backtest and live share the same code path.
7. No look-ahead. Ever.
8. Ambiguity blocks the affected symbol and raises a flag; it never guesses.

Full list: [`ROADMAP.md` §13](ROADMAP.md#13-hard-rules).

---

## Status

Phase 0 — Foundation. **1 / 96** task cards complete.
Next card: [`prompts/P0-02_core_time_timestamp.md`](prompts/P0-02_core_time_timestamp.md).
See [`prompts/LEDGER.md`](prompts/LEDGER.md).
