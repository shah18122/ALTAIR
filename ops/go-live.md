# Go-live checklist and staged capital ramp

**P12-06.** Status: **NOT READY.** This is the list, and the list currently has
blocking items open. It is written so the blockers are visible, not so the
document can be ticked.

---

## 0. What makes a checklist item real

**An item that cannot fail is not a check.** Every line below names the
*evidence* that satisfies it — a command, a file, a number — not a feeling. If
the evidence for an item is "we did that", the item is not done.

The three failure modes this list is built against, all of them observed during
development of this repo:

- **A number that nobody recomputes.** The LEDGER's progress line read `14 / 115`
  for weeks while nine phases closed underneath it. Same class of failure as a
  dashboard literal that cannot track a fit.
- **Absence read as zero.** An empty CSV, a missing rate, a silent index volume
  of 0, a bid of 0.00 that means "no quote". This has appeared in six distinct
  places in this codebase. Assume there is a seventh.
- **A plausible wrong number.** The one that costs money, because nothing about
  it looks wrong.

---

## 1. BLOCKING — nothing goes live with any of these open

| # | Item | Evidence | State |
|---|---|---|---|
| 1.1 | **Rotate the exposed API secrets** | New secret issued at the Kite console; the three exposed during development are dead | ❌ **OPEN** — three secrets were pasted into a development transcript |
| 1.2 | **`charges.toml` verified against a real contract note** | `last_verified` holds an ISO date and `verified_by` a name | ❌ **OPEN** — the file says `UNVERIFIED` |
| 1.3 | **Phase 0 gate: clean build under clang** | `-Wall -Wextra -Wpedantic -Wconversion`, zero warnings | ❌ **BLOCKED** — no clang on this box, no WSL distribution, shell not elevated |
| 1.4 | **Live feed subscription** | A full-mode WebSocket delivering depth, not candles | ❌ **OPEN** — `feed/` decoders are built and tested; nothing subscribes |
| 1.5 | **One full day of reconciliation, green** | `note_reconcile` against a real contract note, `ok() == true` | ❌ **OPEN** — no contract note exists |
| 1.6 | **`block_on_unverified_schedule = true`** | Confirmed in `config/charges.toml` | ✅ set — and it is what makes 1.2 fail loudly instead of silently |

Item 1.2 is the one to take most seriously. The cost model is load-bearing:
rule 5 prices every signal net of cost *before the signal exists*, so an
unverified rate does not produce a wrong report, it produces wrong trades. P12-04
measured the shape of a single stale rate — one head, one side, **₹470.93 on one
sell of 65 NIFTY futures**. Twenty such trades a day is ₹9,400 of edge that was
never there.

---

## 2. Correctness gates

| # | Item | Evidence |
|---|---|---|
| 2.1 | All tests green | `ctest --preset default` — **93/93 at time of writing** |
| 2.2 | Zero warnings at `/W4` | The build is clean; see 1.3 for the other compiler |
| 2.3 | Conservation holds every tick | `Σ(fills) + Σ(costs) + cash_delta == 0` in paise; a breach trips the kill switch |
| 2.4 | The UI cannot trade | Gate 3 link audit: the `desktop/` target does not link `altair_oms` or `altair_broker` |
| 2.5 | No lot size, tick, strike step or expiry is a literal | All read from `instruments/`; `config/lot_size_history.csv` records the revisions |
| 2.6 | Backtest and live share one code path | Replay and live feeds emit the same struct into the same pipeline |
| 2.7 | Every live decision is reproducible | `{model_hash, feature_version, config_hash, spec_version, tick_seqno}` recorded per decision |

---

## 3. Data gates

| # | Item | Evidence |
|---|---|---|
| 3.1 | Dataset coverage has no silent holes | `verify_coverage` against `config/nse_calendar.csv`; the 2024-09-02/03 gap was a fetch-seam artefact and is closed |
| 3.2 | Absent ≠ zero, everywhere it can be | Index volume is *absent*, not 0 — `reports_volume` on the instrument, `zero_volume_is_absent` on the source |
| 3.3 | Holiday calendar is derived, not typed | `config/nse_calendar.csv`, classified by **session start time** rather than bar count |
| 3.4 | Timezone is IST end-to-end | Expiries formatted in IST; the UTC bug printed 2026-09-28 for the 29th, which is the date a roll is planned around |
| 3.5 | No look-ahead | Purged CV with embargo; a replayer that physically cannot expose a future tick; strategies read time off the tick |

**One documented exception to 3.5:** `app/monitor.hpp` takes wall time. The event
it detects is ticks stopping, and a tick-derived clock freezes at exactly that
moment. The exception is in the file header with its reasoning.

---

## 4. Model gates

| # | Item | Evidence |
|---|---|---|
| 4.1 | Walk-forward only, never random K-fold | Markets are not ergodic; the training harness bans it |
| 4.2 | Reported per regime, not only in aggregate | An aggregate Sharpe over two regimes describes neither |
| 4.3 | Sized on the **lower confidence bound** of edge | A signal whose error bar straddles zero is not a signal |
| 4.4 | Nothing claims implausible accuracy | 10-minute directional accuracy tops out around 52–55%. **Anything claiming 70% is overfit** — treat it as a bug report, not a result |
| 4.5 | The neural tier is not shipped untrained | ❌ **no `.pt` or `.onnx` artefacts exist**; `ALTAIR_ENABLE_TORCH` is OFF. Those strategies do not go live |
| 4.6 | Retraining is shadow → canary → auto-rollback | Quarterly retraining degrades as easily as it improves |

A finding worth carrying into live: a 15-minute edge measured at **+2.75σ** on
one window became **−7.19σ** on 9× the data. The first number was not a lie, it
was a small sample. Do not size on a result that has not survived more data.

---

## 5. Operational gates

| # | Item | Evidence |
|---|---|---|
| 5.1 | Kill switch rehearsed | `touch /var/lib/altair/KILL` actually stops order flow, tested in paper mode |
| 5.2 | Warm restart rehearsed | Kill mid-session, restart, confirm it **refuses** a breached snapshot |
| 5.3 | Monitoring wired to a person | A `Stale` board at 09:20 must reach a phone, not a log file |
| 5.4 | Every gauge has a real staleness budget | An unwatched gauge is one nobody notices the loss of |
| 5.5 | DR runbook read *before* it is needed | [`disaster-recovery.md`](disaster-recovery.md) — and it has never been rehearsed |
| 5.6 | Credentials in environment variables only | `broker/` is the only directory that touches one; `data/kite_session.json` is gitignored |

---

## 6. The staged capital ramp

Phase 12's exit is **live at 10% of intended capital**. That is the *end* of
this ramp, not the start.

| Stage | Capital | Duration | Advance only when |
|---|---|---|---|
| **0 · Paper** | ₹0 | 10 sessions | Every fill the engine *would* have taken is reconciled against the real tape, post-cost. No exceptions, no partial days |
| **1 · Shadow** | ₹0, live orders **not** sent | 10 sessions | The would-be order book matches what the strategy claims, and the modelled slippage matches the observed touch |
| **2 · Canary** | **1%** | 20 sessions | ≥ 1 clean reconciliation per session, zero `BrokerOnly` rows, zero conservation breaches |
| **3 · Ramp** | **5%** | 20 sessions | As above, plus realised cost within 5% of modelled cost |
| **4 · Phase 12 exit** | **10%** | — | 40 consecutive sessions with no unexplained discrepancy |

### Rules that govern the ramp

- **A stage is measured in sessions, not days.** A holiday is not progress.
- **Any conservation breach resets to stage 0.** Not the previous stage. The
  invariant is exact integer arithmetic; a breach means something is wrong in a
  way nobody has explained yet, and "we fixed it" is not the same as knowing
  what it was.
- **A `BrokerOnly` row resets to stage 2 at best.** An unbooked fill means an
  order existed that the engine did not know about, which is the failure mode
  that makes every other number meaningless.
- **Ramp down as readily as up.** The step down is not a punishment, it is the
  cheapest available information.
- **Never skip a stage because the numbers look good.** Good numbers early are
  the expected appearance of a small sample. See §4.

### What a stage does *not* prove

Twenty sessions is one month. It contains one expiry, possibly no earnings
season, and probably one volatility regime. **Passing the ramp is evidence the
plumbing works, not evidence the edge is real.** Those are separate claims and
this checklist only tests the first.

---

## 7. Known-open, non-blocking

Carried so they are not rediscovered as surprises:

- `apply_bps` uses `long double`, which is 64-bit on MSVC. Exact for
  single-trade magnitudes; revisit with scaled-integer arithmetic where
  session-accumulated turnover is involved.
- Margin fetch (P1-07) is blocked on credentials.
- XTS is removed from the plan; the enumerators stay so the point-in-time spec
  store can still read historical rows.
- `client/` (the TypeScript SPA) is retired and not built.
