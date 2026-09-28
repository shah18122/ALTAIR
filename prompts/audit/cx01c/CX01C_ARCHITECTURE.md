# CX-01 — actual architecture and ownership

Reader: claude-lead · Date: 2026-09-15 · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`

**Revision 2026-09-15 ~12:10.** **All 22 review groups and LEAD are complete** (481/481 files, 2026-09-15 ~12:45). The documentation and retired-client groups (G19–G22) changed no runtime architecture. G20 re-decoded all 8 conformance vectors independently and confirmed that `client/src/protocol.ts` matches `server/protocol.hpp`. Facts added since the first draft:
- Nothing in `desktop/*.hpp` runs off the GUI thread (G16). Subprocess waits reach 330 s, 600 s and 45 s in three panels (C18-015).
- The desktop PriceClient reassembly buffer is **unbounded**, because `decode_header` never caps `payload_len` (C18-013). One bad frame can grow memory to about 4 GiB inside the engine process.
- `config/altair.toml` has **no consumers** (C22-003).
- `round_to_tick` and Pool double-release are confirmed (C01-001, C01-004).
- The PlausibilityGate has no staleness bound (C01-006).

Wherever a PROVISIONAL marker below conflicts with these facts, **these facts supersede it**.

**Evidence basis.** This describes what the code **does**, not what ROADMAP §5 designs. Sources are reviewer §5 sections (G02 core, G03 feed/book, G05 features/backtest/flagging, G08 models serving/forecast, G13 OMS, G14 broker tools, G15 app/server, G17 desktop shell checkpoint), lead spot-checks (`LEAD_VERIFICATION.md`), and the LEDGER. Sections marked **PROVISIONAL** depend on groups not yet complete at this revision (G01, G04, G06, G07, G09, G10, G11, G12, G16, G18) and must be re-checked when their findings land.

## 1. The one-paragraph truth

Altair is a large, well-tested **library of primitives** plus four kinds of executable:
- a Phase-0 synthetic replay demo (`altair`)
- a display-only price republisher (`altair_price_service`)
- six single-shot Kite CLI tools
- an in-process Qt desktop that computes models on demand for display

**No executable assembles the ROADMAP §5.1 live path** (ingest → normalise → book → features → models → aggregator → cost → risk → OMS → broker). No component consumes order intents, and no code sends an order to a broker. Every order, risk, feature, book and serving component is exercised **only by tests or by explanatory desktop pages**.

## 2. Executables and processes

| Executable | Built when | Threads | What it actually does | Reads | Writes |
|---|---|---|---|---|---|
| `altair` (`app/main.cpp`) | always | 1 | `--selftest`; `--instruments` (Phase-1 walkthrough); `--gen` / `--replay <.tick>` synthetic ReplayTick session through a **null strategy** and `ConservationLedger` | optional Kite/UDiFF CSV; `.tick` session | `--gen` writes a session file |
| `altair_price_service` | `net` + Boost | 1 (asio `io_context` constructed, **never run**; non-blocking sockets polled) | `--go`: Kite WebSocket (blocking `ws.read`) → `decode_kite_frame` → PriceBus frames on loopback TCP. `--replay SYM IV`: dataset CSV **bar closes** as price frames flagged `kPriceReplay` | `ALTAIR_KITE_API_KEY` env, `data/kite_session.json` (CWD-relative), `dataset/` | TCP 127.0.0.1:7421 |
| `altair_kite_login` | `net` | 1 | request_token + key/secret → `/session/token` | env key/secret, argv token | `data/kite_session.json` (**in place**, C14-010) |
| `altair_kite_fetch` | `net` | 1 | historical candles, `--dump-instruments` | session | `<out>/YYYY-MM.csv` (**6 sig. digits**, C14-001), master CSV (atomic) |
| `altair_kite_update` | `net` | 1 | tops up 15 NIFTY/BANKNIFTY/VIX series | session | `dataset/spot/*/*/*.csv` (`%g`, C14-001; rename unchecked, C14-005) |
| `altair_kite_account`, `_quote`, `_ticker` | `net` | 1 each | profile/margins/positions; quotes; bounded WebSocket sample | session | `data/kite_account.json`, `data/kite_quotes.json` (drops OI etc., C14-008), `data/kite_ticks.json`, optional tick store (ticks only) |
| `altair_desktop` | Qt found | Qt GUI thread, plus QProcess children and one QTcpSocket | 33 nav pages; fits and reports **synchronously on the GUI thread** (C17-001); Terminal subscribes to the price service; spawns all six Kite tools | `dataset/`, `data/instruments.csv`, `config/*.toml`, `data/kite_*.json` (metadata/fields) | `data/order_intents.jsonl` (append), `data/kill_request.json` |
| test binaries (130–135), `altair_bench_chain`, `altair_emit_vectors` | tests / bench | tests spawn threads only in concurrency tests | — | — | `emit_vectors` rewrites `server/tests/vectors/frames.txt` |

Cross-process shared state is **files and one loopback socket**: the session token, quote/tick/account snapshots, the intent queue, the kill-request file, dataset CSVs, and the PriceBus TCP stream. There is no shared memory and no IPC beyond these.

## 3. Data flow: designed versus connected

```
DESIGNED (ROADMAP §5.1)                     CONNECTED TODAY
instrument masters → reconciler → spec      kite_dump parser + Reconciler + SpecStore: `altair --instruments`
store (point-in-time)                       demo; desktop master lookup / chain (real parser).
                                            Live decode paths register PLACEHOLDER specs
                                            (lot 1, tick 1, scale 100) — C03/C15 leads.
Kite WS → decoder → normaliser → failover   decoder → PriceBus (display) or → TickStoreWriter (ticks only).
→ SPSC → L2 book → OBI/VPIN/λ               Normaliser, FailoverWatchdog, L2Book: TESTS ONLY (C03-002).
→ analytics → feature registry              Feature builders: TESTS ONLY; registry: desktop + research templates.
→ models (warm, pinned) → aggregator        Desktop refits GBDT/conformal/Markov/etc. on click; ModelServer,
                                            ModelRegistry: TESTS ONLY; nothing persisted (G08).
→ cost → sizing → hard limits → OMS         Cost/sizing/limits: tests + desktop pages (PROVISIONAL, G12).
→ router → Kite adapter → state machine     OMS: TESTS ONLY; kite_adapter builds bodies, no transport (G13).
→ exit ladder → reconciliation              UI writes intents; NOTHING drains them (C13-009).
→ flagging/drift → retrain                  drift/scorecard: desktop page only; deploy (shadow/canary): tests only.
→ UI (binary WS)                            In-process Qt reading files and the local PriceBus.
```

## 4. Thread ownership and shared state

- **Engine libraries create no threads** and hold no locks. Every primitive is single-owner by convention: SpecStore, Normaliser, L2Book, ConservationLedger, order state, schedules and throttle.
- Multi-thread designs exist and are **exercised cross-thread only in tests**:
  - `SpscRing` (feed→strategy)
  - `MpscRing` (strategies→OMS; stalled-producer wedge C02-007)
  - `SeqlockSnapshot`/`ConfigStore` (hot reload; data race C02-002; unbounded spin C02-003)
  - `ModelServer` (two slots; reader-lifetime and generation races C08-001/002)
- In the only engine executable (`altair`), config publish/refresh, logger, log decoder and ledger all run on **one thread**.
- `altair_price_service` is single-threaded: a blocking WebSocket read interleaved with non-blocking bus flushes. A slow client never stalls publishing; it is coalesced (C15-001 partial-frame defect).
- Desktop: **all widget logic, model fits, chain repricing and socket handling run on the Qt GUI thread** (C17-001, C17-007). No worker threads were found by G17 in the files it read (PROVISIONAL for G16/G18 files). QProcess children are separate OS processes. Because the engine and UI share one process (CLAUDE.md in-process decision), a blocked GUI thread blocks any future in-process engine work on that thread.

## 5. Queue boundaries, capacities and overflow policies

| Boundary | Capacity | Overflow policy | Status |
|---|---|---|---|
| `SpscRing<T,N>` | power of two, all slots | refuse (return false), never overwrite | tests only |
| `MpscRing<T,N>` | power of two | refuse; a stalled reserver wedges both ends (C02-007) | tests only |
| Binary log ring (`altair`) | 4,096 records | drop newest, count attempts, mark gap (saturates 65,535); drops never printed (C02-010) | `altair` only |
| `LogRegistry` | 1,024 sites | returns `kInvalidSite`, unchecked by `Logger` | — |
| `ConfigSnapshot` | 128 entries, 47-char keys | refuse | `altair` only |
| `SpecStore` | 8,192 instruments; token index per source | refuse; universe filter runs before `add` (P1-09) | demos, desktop |
| Kite decoder output | caller-sized (512 ticks / 512 depth in both apps) | `OutputFull` all-or-nothing; `ShortFrame` loses decoded packets (C03-009) | price service, ticker |
| Normaliser | — | future/inactive dropped and counted; stale published flagged | tests only |
| `L2Book` | 8,192 × `BookState` (272 B) | bad instrument/stale seq reject+count; crossed store+mark; **level counts unchecked** (C03-005) | tests only |
| `Vpin` | 64 buckets | silent clamp (C03-010) | desktop demo |
| PriceBus per client | 1 MiB outbox + 64 KiB `SO_SNDBUF`; clients unbounded | drop oldest whole frames and count; **can drop a partly written frame** (C15-001, V-09) | price service |
| Order intent queue (`data/order_intents.jsonl`) | unbounded file, no expiry, no id dedupe | parser counts rejected lines; torn-line merge (C13-005, V-04) | written by UI, **drained by nothing** |
| `ModelRegistry` | 128 records, 40-char names | names truncated silently (C08-015) | tests only |
| Desktop PriceClient read buffer | 256 KiB (Terminal) | PROVISIONAL (G18) | desktop |

## 6. Hot versus cold paths

- **Marked `ALTAIR_HOT`**, and noexcept/allocation-free per reviewers:
  - core: SPSC/MPSC push/pop, seqlock store/load, `ConfigStore::refresh`, snapshot lookups, `Logger::write`, `ConservationLedger::on_fill/check`
  - feed/book: `decode_kite_frame`, `Normaliser::submit`, `FailoverWatchdog::on_data`, `L2Book::apply`, microstructure estimators, `Replayer::next`
  - models: `ModelServer::infer`
  - oms: `apply`, `apply_fill`, `evaluate_exit`
  - analytics: `price_chain` (lead P39-00)
- **Hot-path hazards:**
  - unbounded spins with no pause (`SeqlockSnapshot::load`, `ConfigStore::refresh`, C02-003)
  - an unbounded loop in `Vpin::on_trade` (C03-010)
  - UB-level races in the seqlock (C02-002) and ModelServer (C08-001)
- **Cold, and allocating by design:** instrument parsing, TOML loading, intent parsing, Kite REST/body handling, model training/evaluation, conformal calibration, backtests, Monte Carlo, desktop pages.
- **No hot path is connected to a live event source**, so hot-path budgets are measured only in isolation (`CX01C_BASELINE_REPORT.md` §5).

## 7. Conventions

| Dimension | Convention in code | Known deviations |
|---|---|---|
| Time | `Timestamp` = i64 ns since Unix epoch, UTC; `Duration` affine | Kite exchange_ts is **1 s** resolution; LTP/quote modes carry none (`exchange_ts = recv_ts`, `NoExchangeTs`). `recv_ts` is documented TSC/monotonic, but the executables use `system_clock` (C03-002). Session windows and expiries are **IST** (expiry = IST midnight + 15:30 settlement); the tick store's session date uses now() (C03-003) |
| Price | i64 **paise**; Kite WS integers at scale 100; REST prices are decimal **rupees** via `kite::kite_paise_to_rupees` | CD segments refused (finer than a paisa). Dataset CSVs are decimal rupees at **6 significant digits** (C14-001). PriceBus pads missing depth with price-0 levels (C03-012) |
| Quantity | `Qty` signed units (+buy/−sell); `Lots × LotSize → Qty`; intents carry **lots** | TWAP/VWAP/POV split raw units with no lot multiple (C13-014) |
| Money | integer paise; `notional_of` overflow-checked | StrongInt `+`/`-` wrap unchecked (G02); `round_to_tick` / `apply_bps` overflow leads (Codex E001/E002, G01 PROVISIONAL) |
| Identity | `InstrumentId` = dense SpecStore **insertion index**; broker token per `FeedSource{Kite=0,Xts=1}` (`kFeedSourceCount=2`) | `ReplayTick` carries a raw broker token. Executables recover ids by insertion order with placeholder specs. The recorded tick store has no spec version or token map (C03-004). Two unrelated `FeedSource` enums (`altair::` vs `altair::ui::`) |
| Sequence | u32 per Normaliser shared by ticks+depth; PriceBus u64 per topic | no session epoch (C03-013) |

## 7a. Model training and publication (summary; detail in `CX01C_IDEA_ATLAS_READINESS.md` §5)

- **Nothing is trained once and kept.** No weights, no registry file, no `.pt`/`.onnx` artefact (LEDGER P12-06; G08).
- The desktop refits models **in memory on each click**; tests refit on every run.
- `ModelServer` (hot-swap contract) and `ModelRegistry` (Registered→Shadow→Canary→Live) are unconnected to each other and to any executable (G08).
- Config publication: `ConfigStore` over a seqlock. Publish is single-threaded today; versions are not enforced (C02-004); handles are bare indices (C02-005). `charges.toml` is loaded only where tomlplusplus exists (`net`/`vcpkg` presets).

## 8. Live data vs recorded ticks vs historical bars

| Path | Source → sink | Decision path? |
|---|---|---|
| A. `ReplayTick` + `Replayer` | synthetic `.tick` (`altair`); desktop `real_tape` converts dataset 1-min bars to ReplayTick stamped at **bar end** | `altair`: null strategy + ledger. Desktop: grid/chart display |
| B. Live canonical `Tick`/`DepthUpdate` | Kite WS → decoder → PriceBus (price service) or → TickStoreWriter (ticker; ticks only, no depth) | **none** |
| C. Recorded canonical ticks | `TickStoreReader` | **tests only**; no cursor, no replay adapter, identity unresolvable |
| D. CSV bar replay | price service `--replay`: bar close → PriceBus with `kPriceReplay`, stamped `b.ts_ns` (possibly bar START, so one interval early vs path A — G03 hypothesis) | **none** (display) |

**Hard rule 6** ("backtest and live share the same code path") is **not demonstrable today**. `backtest/engine.hpp` defines a tick-level strategy concept that no strategy implements (G05), and live ticks reach no decision code.

## 9. Ownership boundaries

- **Order placement:** by construction, only `oms/` could place an order, and today nothing can. `desktop/` links neither `altair_oms` nor `altair_broker` (gate-3 allow-list). The desktop nonetheless has filesystem write access (intents, kill request), network access (loopback PriceBus) and process-spawn ability (all six Kite tools, which hold the session token). "The UI cannot trade" holds because **no drainer exists**. It is a property of the build graph and file conventions, not of a security boundary (Codex UI review; G14).
- **Credentials:** designed to live only in `broker/`. In practice six `app/` tools read key/secret from env and five copies of a session-file reader exist (C14-011). The session file is written in place without restricted permissions (C14-010).
- **`server/` vs retired `client/`:** a shared conformance vector file, no shared code. `server/CMakeLists.txt` comments still describe uWebSockets and a TypeScript client (G15).
