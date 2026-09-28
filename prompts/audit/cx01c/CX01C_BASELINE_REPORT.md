# CX-01 — build, test and latency baseline

Reader: claude-lead · Runs: 2026-09-14 20:52–21:47 IST · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`
All raw logs: `prompts/audit/cx01c/baseline_logs/`. **No source was changed.** No thresholds were edited, and no failing run was retried to get green (none failed).

## 1. Environment

| Item | Value |
|---|---|
| Hardware | Intel Core i5-8500T, 6 cores / 6 threads, 2.1 GHz nominal; 15.9 GB RAM |
| Cache (OS-reported by `bench_chain`) | line 64 B, L1d 32 KB, L2 256 KB, L3 9,216 KB |
| OS / power | Windows 10 Pro 10.0.19045; power plan **Balanced** (not High Performance) |
| Compiler | MSVC `cl` 14.51.36231 (19.51), `C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools` |
| Build tools | CMake/ctest 4.3.1-msvc1 and Ninja, both from VS Build Tools |
| Qt | 6.8.3 msvc2022_64 at `D:/Qt` |
| vcpkg | `C:/PycharmProjects/vcpkg` (`net` preset: features `config;net`) |
| Flags | `default`/`net`: RelWithDebInfo `/O2 /Ob1 /DNDEBUG`. `default` cache shows `ALTAIR_NATIVE_ARCH=ON`, `ALTAIR_BUILD_BENCH=OFF`, `ALTAIR_ENABLE_TORCH=OFF`; `net`: `ALTAIR_BUILD_BENCH=ON` |
| Working tree | Only untracked audit/Codex/user files before and after every run; **no tracked file modified** |
| Background load | Idle Codex desktop processes; Claude reviewer subagents (network-bound, reading files). No build or test ran concurrently with the latency measurements. Reviewer subagents were running during the ASan run |

## 2. Test side effects inspected before running

| Test | Side effect | Risk |
|---|---|---|
| `order_intent` | writes and removes `test_intents_tmp.jsonl` in the ctest working dir | none if suites are not run concurrently |
| `tick_store` | writes and removes `altair_tick_store_test.bin` in the working dir | same |
| `warm_restart` | fixed-name files in `%TEMP%` (`altair_snap_*.txt`) | same |
| `protocol_vectors` (`emit_vectors`) | **rewrites tracked `server/tests/vectors/frames.txt`** (`ALTAIR_VECTOR_DIR` = source tree; G15 C15-003) | tracked file; checked with `git status` after each run: unchanged |
| `price_bus`, `desktop_price_client` | bind loopback ephemeral ports | none |
| desktop tests | temp dirs for synthetic session files; `desktop_terminal` reads only the size of the real `data/order_intents.jsonl` | no writes to `data/` observed; `data/` contents not opened by the lead |
| broker tests | constants and parsers only; no network | none |
| `config` / `toml_source` | reads the live `config/altair.toml` (WORKING_DIRECTORY = source dir) | read-only |

Suites were run **sequentially**, never two ctest runs at once.

## 3. Checks run, and their results

| # | Check | Command (repo root) | Exit | Result | Evidence |
|---|---|---|---|---|---|
| B1 | Configure `default` (inherited Codex run, 11:24) | `cmake --preset default` | 0 | Skipped by design: tomlplusplus (P0-08b TOML loader), broker net half (no OpenSSL), P3-09b charges loader, price-bus test (no Boost). Qt found. **DEV CREDENTIALS compiled in** (admin/admin, staff/staff). Benchmarks OFF, ML tier OFF, invariants ON. Namespace/bounds/atlas audits clean (615 names; 8 clamp sites; 85 models 59/8/18) | `prompts/audit/CX01_CONFIGURE_DEFAULT_RETRY.txt` (Codex; its first attempt's exit 1 was a shell-quoting error, not configure) |
| B2 | Build `default` | `cmake --build --preset default` | 0 | `ninja: no work to do`: binaries already current for these sources. **Nothing compiled today**, so the zero-warning gate was not re-observed for this preset | `build_default.log` |
| B3 | Test `default` | `ctest --preset default` | 0 | **130 / 130 passed**, 211.04 s. Slowest: `models_rich_features_experiment` 50.0 s, `backtest_validation` 45.8 s, `models_neural_5m` 37.0 s | `ctest_default.log` |
| B4 | Build `net` | `cmake --build --preset net` | 0 | `ninja: no work to do` (same caveat as B2) | `build_net.log` |
| B5 | Test `net` | `ctest --preset net` | 0 | **135 / 135 passed**, 212.36 s. Adds `charges_toml`, `toml_source`, `price_bus`, `desktop_price_client`, `kite_ticker` (all offline) | `ctest_net.log` |
| B6 | Configure `tsan` | `cmake --preset tsan` | **1** | FATAL by design: "MSVC has no ThreadSanitizer … Run this preset on Linux with GCC or Clang" (`CMakeLists.txt:128`) | `configure_tsan.log` |
| B7 | Build `asan` | `cmake --build --preset asan` (env `CMAKE_BUILD_PARALLEL_LEVEL=4`, per LEDGER C1060 note) | 0 | **Actually compiled**: 159 steps, 10 links, **0 compiler warnings** | `build_asan.log` |
| B8 | Test `asan` | `ctest --preset asan` | 0 | **129 / 129 passed**, 2,434.30 s. The preset excludes label `experiment` (`models_rich_features_experiment`). No sanitizer abort. Slowest: `desktop_pages` 540 s, `backtest_validation` 455 s, `models_spot_forecast` 399 s, `models_neural_5m` 362 s | `ctest_asan.log` |

## 4. Gates not run or unavailable

| Gate | Status | Why |
|---|---|---|
| ThreadSanitizer / race freedom | **UNVERIFIED** | MSVC has none (B6). No Linux environment; WSL is not enabled (LEDGER Toolchain). **Windows ASan is not evidence of race freedom.** |
| UndefinedBehaviorSanitizer | **UNVERIFIED** | The `asan` preset says "Address/UB", but MSVC offers AddressSanitizer only. Language-level UB (e.g. unchecked signed overflow C13-006, the seqlock race C02-002) is not detected by any run here |
| Leak detection | **UNVERIFIED** | Not confirmed to be active in MSVC ASan on this build |
| Linux GCC/Clang build, `-Wall -Wextra -Wpedantic -Wconversion` | **UNVERIFIED** | No toolchain. Portability hypothesis V-03 (`std::atomic_thread_fence` is an MSVC STL facility) could block it |
| `debug`, `prod`, `vcpkg` presets | **Not run in this audit** | `vcpkg` is a subset of `net`. LEDGER P12-07 records all seven presets green earlier; that is inherited and not re-verified today |
| Zero warnings on `default`/`net` | **Not re-observed** | No-op builds (B2, B4). Only `asan` compiled (0 warnings) |
| `client/` (retired TypeScript) | **Not run, deliberately** | Do not install or revive |
| Live broker, live feed, orders | **Not run, deliberately** | Audit only |

## 5. Latency measurements

**Boundaries.** Every number below was produced **inside a unit-test or benchmark binary**:
- one process, no CPU affinity, Windows scheduler, Balanced power plan
- `net` preset RelWithDebInfo `/O2 /Ob1`
- the logs mostly print a **batch-timed mean** (total time / operations), not a percentile; only `tsc_clock` prints p50/p99
- sample counts are the test's own and mostly not printed
- none is p99.9
- none is an end-to-end pipeline figure, because no pipeline is assembled
- nothing includes broker network, exchange or Qt paint time

Collected 2026-09-14 21:03, one run each; `bench_chain` three runs. Raw: `latency_summary.log`, `latency_<test>.log`, `bench_chain_run{1,2,3}.log`.

| Stage / primitive | Measured (method) | Budget | Note |
|---|---|---|---|
| `TscClock::now()` | corrected p50 28 ns, **p99 32 ns** (percentile over 10,000 reads, harness-subtracted) | p99 < 25 ns | **Printed "OVER BUDGET — review gate 6" and the test still exited 0.** LEDGER P0-03 recorded 18 ns on a quieter run. Shows C02-006: budgets do not fail ctest |
| `Arena::allocate(32,8)` / `Pool` acquire+release | 2.51 ns / 1.57 ns (batch mean) | 10 / 15 | |
| `SpscRing` push+pop | 9.02 ns (batch mean); 2-thread stress 131.5 M elem/s | 20 | LEDGER 2.48–2.80 ns; this run is ~3× slower |
| `MpscRing` push+pop; seqlock store / try_load | 16.94 / 3.83 / 2.20 ns | 40 / 20 / 20 | Seqlock numbers are uncontended |
| `Logger::write`; `LogDecoder::decode_line` | 31.87 ns; 229.81 ns | 40; none | |
| `ConfigStore::refresh` unchanged; `get_int(handle)` | 2.27 ns; 1.43 ns | 5; 5 | |
| `ConservationLedger::on_fill`; `check_and_trip` | 12.84 ns; 0.32 ns | 20; 20 | The check is tautological (C02-001) |
| `SpecStore::id_of` worst case (2,000 specs); `at(id,when)` | 1.23 ns; 2.62 ns | 20 | |
| `Replayer::next()` | 2.48 ns | 10 | Synthetic Phase-0 replayer |
| Kite binary decode | 711 ns per 10-packet frame = **71 ns/packet** (mean) | ROADMAP §11 wire→tick 1 µs p99 (test compares against 3 µs) | Mean only |
| `Normaliser::submit` | 11.4 ns (0.059 s / 5.12 M) | — | Timer printed to 1 ms, so resolution ≈0.2 ns per unit |
| `L2Book` apply | 29.4 ns (0.060 s / 2.05 M) | ROADMAP §11 0.5 µs | Same resolution limit |
| IV solve (mixed corpus) | **1,591 ns/solve**; a 100-strike chain = 159 µs | ROADMAP: full chain greeks **+ IV** < 50 µs | **The IV part alone is ~3× the full-chain target** if every strike is solved per update |
| Rolling window push+variance (N=256); `RunningMoments::add` | 652 ns; 7.3 ns | 5 µs; 200 ns (test's own) | |
| `Ewma::update` / `EwmaVariance::update` | 14.9 / 16.9 ns | 100 / 150 ns | |
| Kite dump parse | 2.74 M rows/s | pre-open only | |
| Black-76 chain sweep, SoA (`bench_chain`, 3 runs) | **227–239 ns/strike** (mean); AoS 231–247 | ROADMAP: full ~200-strike chain 50 µs p99; single option 0.3 µs | 200 strikes ≈ **45–48 µs mean** (p99 not measured, and the margin is under 10%). Single option ≈ 0.23 µs mean |
| *`bench_chain` methodology caveats (reviewer G06)* | Each cell is **one mean from a single ≥250 ms run** after a one-call warm-up; no p50/p99; the clock call sits inside the timed loop; the AoS row is padded to 128 B, so it touches 1.77× the memory, which **biases the sweep toward SoA** | — | The P39-00 commit message (lead-authored, this session) compared this mean with the ROADMAP §11 **p99** budget ("46 µs vs 50 µs"). That comparison is **not valid** and is withdrawn here |
| Strike lookup by cache tier, SoA vs AoS | L1 2.6 vs 2.6 · L2 4.3–4.4 vs 3.3–3.4 · L3 17–18 vs 10–12 · RAM 82–93 vs 21–22 ns | — | AoS wins single-strike lookup up to ~4× in RAM |

Invalid sample, excluded: `app_selftest`. My harness mapped it to the wrong executable, and its log repeats `models_serving` output. It was not re-run under ASan contention. The Phase-0 `altair --replay` throughput (LEDGER 54 M ticks/s) was **not** re-measured, because `--gen` writes under `data/`.

`models_serving` printed its intentional in-place-swap demonstration: "6 concurrent reads: 1 TORN [344,279 writes]". That is the demo of the hazard the double-buffer exists to prevent, not a failure. The double-buffer's own reader-lifetime defect (C08-001) is **not** exercised by that test (C08-023).

## 6. What these baselines do and do not establish

- **Established:** at `a34af5c`, the `default` (130), `net` (135) and `asan` (129) test suites pass on this Windows box. The ASan build compiles warning-free. Primitive-level mean latencies are generally inside their own budgets, with the named exceptions: TSC p99 over budget, and IV-per-chain far over the ROADMAP chain target.
- **Not established:**
  - race freedom
  - UB freedom
  - Linux buildability
  - p99/p99.9 tail latency for any stage
  - end-to-end ingress-to-decision latency (no assembled path exists)
  - broker round-trip
  - UI responsiveness under a stream
  - that ROADMAP §11 targets are met
- **Test-suite caveats (from reviewers):**
  - latency budgets print and never fail (C02-006)
  - some concurrency tests SKIP yet PASS (C08-023)
  - the conservation check cannot fail (C02-001)
  - `protocol_vectors` rewrites a tracked file (C15-003)

A green ctest therefore says less than it appears to.
