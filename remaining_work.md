inecl Altair — Remaining Work

Reconciled against `final.md`, source, build output and tests on 2026-09-27.
This file is the truthful boundary between work completed locally and evidence
that requires external hardware, entitled accounts or licensed data.

## Status

| Item | Count | State |
|---|---:|---|
| Completed parent task cards | 44 | Implemented at their stated scope |
| Open parent task cards | 10 | P1-01..P1-06 and P8-01..P8-04 |
| Open operational evidence gates | 5 | Live/account/licensed verification |
| Model child cards | 27 | 27 numerical implementations, 0 partial, 0 absent |
| Carried FYERS identity debt | 0 | Token width and ISIN matching are fixed |

Verification baseline: the production Windows desktop built and staged, and the
expanded complete `build/net` CTest suite passed **204/204** on 2026-09-28
(75.81 s, four-way parallel). The earlier pre-expansion baseline was 188/188.

Live order submission remains disabled by design. A local unit/integration test
does not substitute for credentialed broker evidence, licensed market data, or
native Apple Silicon measurements.

## 1. Open parent cards

### Phase 1 — Mac portability and measured GPU feasibility

The portability code can be reviewed on Windows, but these completion criteria
require native Apple Silicon evidence.

| ID | Required completion evidence | External prerequisite |
|---|---|---|
| P1-01 | Configure and compile the native macOS presets | Apple Silicon Mac |
| P1-02 | Exercise Darwin allocation/deallocation paths | macOS/arm64 |
| P1-03 | Validate the supported monotonic clock path | macOS/arm64 |
| P1-04 | Build, stage and launch the desktop/helpers bundle | macOS |
| P1-05 | Measure the same training workload on CPU and MPS | Apple GPU/MPS |
| P1-06 | Publish the evidence-based Mac go/no-go result | P1-01..P1-05 |

### Phase 8 — End-to-end acceptance and handoff

The Windows-local tier is complete and recorded in
`ops/phase8-acceptance.md`, including native UI evidence, concurrency/recovery
tests, the staged desktop and a hashed ZIP package. The rows below are the
remaining *external* tier only.

| ID | Required completion evidence | External prerequisite |
|---|---|---|
| P8-01 | Both-disconnected, FYERS-only, Kite-only, reconnect and stale-session broker scenarios | Entitled broker accounts/credentials |
| P8-02 | Full UI checklist against live-updating account/feed state | Live feed plus rebuilt desktop |
| P8-03 | Measured concurrency/latency and live-fed paper-session recovery | Entitled live feed and paper run window |
| P8-04 | Installable builds and tier-by-tier completion record | P8-01..P8-03 plus macOS evidence for Mac tier |

These ten cards are the only unchecked parent cards in the Phase 0–8 backlog of
`final.md`.

### Roadmap-only open work not in the Phase 0–8 backlog

`final.md`'s ROADMAP source section carries work the ten-card count above does
not cover. It is listed here so the boundary is not understated:

| Item | State | Evidence |
|---|---|---|
| P11Q-07 | **TODO** — fit the models that can be fit on existing data | marked TODO in the Phase 11Q table |
| P11Q-08 | **TODO** — training harness with a per-model walk-forward split | `models/training.hpp` exists, but the roadmap card is unmarked |
| P12-01..P12-06 | **Not started** — Linux deployment, supervision/warm restart, monitoring, reconciliation, DR runbook, go-live ramp | `ops/linux-deployment.md` self-declares "PLAN, NOT PROCEDURE. Altair has never been compiled on Linux." |
| Phase 9 (P9-01..P9-07) | **Implemented-but-unreconciled** — flagging, drift, shadow/canary/rollback, quarterly scheduler | `flagging/drift.hpp`, `flagging/deploy.hpp`, `models/registry.hpp`, `flagging/scorecard.hpp` |
| Phase 10 (P10-01..P10-08) | **Implemented-but-unreconciled** — fundamentals, DCF, cointegration, pair selection, neutralisation, VIX, vega | `models/dcf.hpp`, `strategies/cointegration.hpp`, `strategies/fundamentals.hpp`, `strategies/vix_forecast.hpp`, `risk/hedge.hpp`, `models/named_factors.hpp`, `risk/neutralise.hpp` |

"Unreconciled" is deliberate: the deliverables are on disk but their Phase 9/10
exit criteria (`final.md` ROADMAP §12) have not been re-verified and the roadmap
table has no status marks. Do not read their presence as completion.


## 2. Operational evidence gates on locally completed cards

The implementation cards below are checked, but their live acceptance evidence
is intentionally still open.

| ID | Local implementation | Remaining evidence |
|---|---|---|
| P2-09 | Typed broker publication bridge | Run against an entitled live account |
| P3-02 | Bounded FYERS decoder/adapter | Credentialed callback and socket session |
| P3-03 | Epoch-aware source router | Live supervisor/publication run, including reconnect and stale epochs |
| P3-05 | Read-only FYERS history audit | **Live verified 2026-09-28:** 750 NIFTY 1m bars fetched; 588 overlapped local history, 4 close differences, max 0.15 points. Broader instruments/ranges remain an evidence expansion, not an implementation blocker. |
| P7-06 | MTBT feed specification | Licensed decoder and licensed-feed validation |

## 3. Model implementation queue

P6-03 and P6-04 are implemented and tested. P6-05 and P6-06 are complete as
parent deliverables. Their child queue has advanced from specification-only to
27 numerical implementations, zero implementation gaps, and zero absent models.
This does **not** claim the algorithms are market-trained or validated.

- `prompts/P6-05_ALGORITHM_CARDS.md` contains the per-card status and evidence
  for 27 cards covering SVM,
  KNN, autoencoder, ARMA/ARIMA/SARIMA, EGARCH, OU, Heston, trinomial/PDE,
  MLP/LSTM/GRU/Transformer/causal CNN, stat-arb and factor models, queue/fill/
  Hawkes/event models, agent simulation and DQN/PPO/actor-critic.
- `prompts/P6-06_DATA_GATES.md` records the order-depth, labelled-fill,
  point-in-time fundamentals, licensed-news, RL-environment, seasonal-history,
  option-surface and Apple-MPS gates.
- Synthetic fixtures may verify mechanics; they cannot be reported as market
  validation or live model performance.

M12..M15 are now complete: full MLP backpropagation, analytic BPTT for LSTM and
GRU, and a causal multi-head stacked Transformer reference with complete
checkpoints. Data-gated rows remain explicitly non-market-validated even where
their numerical mechanics are green.

## 4. Locally finished since the previous checklist

- Phase 4 Terminal is account-first: broker-attributed funds, comparable but
  explicitly non-fungible total, filterable/sortable positions and secondary
  operational controls.
- P6 background jobs support progress, cancellation and generation-safe result
  publication; model-family results no longer depend on raw report dumps.
- P7 Arbitrage has a bounded producer/UI handoff and opportunity workspace.
- P7 paper sessions use the isolated paper venue, journal/recover deterministically,
  publish typed funds/positions and retain partial multi-leg outcomes visibly.
- FYERS and Kite order translation remain behind the shared disabled dispatch
  permit; uncertain FYERS submissions require reconciliation and are never
  blindly retried or failed over.
- FYERS `fy_token` uses `uint64_t`; NSE/BSE matching uses ISIN, not symbol or
  underlying guesses.
- Costed parity/calendar reports now refuse an absent or unverified charge
  schedule at the page boundary; no partial cost output is rendered.
- Atlas now has 85 entries: **85 implemented, 0 partial and 0 absent**. Added
  deterministic SVM/KNN/linear-autoencoder, ARMA/ARIMA/SARIMA/OU, option
  pricers, factor risk/named factors, queue/fill/Hawkes/event models, agent
  simulation, DQN/PPO/actor-critic and a constrained stat-arb basket.
- Added causal, costed pairs-trade lifecycle/P&L and point-in-time sentiment
  scoring, then closed M12..M15 with tested full-parameter neural trainers.

## 4a. Verification findings (2026-09-27)

Local checks that changed a status or exposed a defect. None of these is a live,
credentialed or hardware result.

- **`price_bus` hung instead of failing under parallel CTest.** `ctest -j 4` over
  `build/net` completed 198/199 and left `price_bus` blocked with zero CPU;
  isolated it passes in 3.32 s. Cause: a blocking read with no deadline at
  `server/tests/test_price_bus.cpp` waiting for exactly `kN * frame_bytes` and
  never returning if a frame is late. Now bounded (10 s) and it reports a
  timeout instead of hanging.
- **Open audit findings, still with no card:** C22-002, C22-003 (no C++ code
  reads any key from `config/altair.toml` — re-confirmed), C22-005, C22-006,
  C01-006, C01-008.
- **Benchmark tier inconsistency:** `.commandcode/taste/tooling/taste.md` states
  "Uses Google Benchmark", but `CMakeLists.txt` has the `find_package(benchmark)`
  line commented out; only `analytics/bench` builds today, and `ALTAIR_BUILD_BENCH`
  defaults ON while every preset except `vcpkg`/`net` sets it OFF.

## 5. Completion rule

`remaining_work.md` is finished as a reconciliation artifact: no locally closed
item is left listed as open, and no external prerequisite is mislabelled as
complete. The project itself is not globally certified until Sections 1–3 have
their required implementation, hardware, data and live acceptance evidence. Any future closure
must update `final.md`, this file and `change_by_codex.txt` together with the
exact build/test or operational evidence.
