# Disaster recovery and position flattening

**P12-05.** Status: **WRITTEN, NEVER REHEARSED.** No step below has been
executed against a live broker account, because none has existed. Treat every
timing here as an estimate and rehearse it in paper mode before it is needed.

---

## 0. The rule that outranks the rest of this document

**You cannot flatten what you do not know you hold.**

Every instinct under pressure is to reach for the flatten button first. That is
wrong, and it is wrong in a specific way: flattening acts on the *engine's* idea
of the position. If a fill arrived during the crash and was never booked —
P12-04 calls that BROKER-ONLY, and it is the dangerous outcome for exactly this
reason — then flattening the engine's position leaves the real one open, and it
can leave it open *doubled*, because the flatten order is itself a new position
against a holding the engine cannot see.

So: **stop, then look, then act.** The stop is instant. The look takes minutes.
Skipping the look to save those minutes is how a bad afternoon becomes a bad
quarter.

---

## 1. Immediate — stop new orders

> **Corrected 2026-09-29 (audit C22-005).** This section used to say
> `touch /var/lib/altair/KILL`, and `config/altair.toml` says
> `kill_switch_file = "run/KILL"`. **No code watches either path.** What exists
> today is:
>
> - `risk::KillSwitch` (`risk/limits.hpp`), in process. It is tripped by, for
>   example, a reconciliation mismatch (`oms/reconcile.hpp`) and checked by the
>   exit ladder (`oms/exit_ladder.hpp`).
> - The desktop **Kill Switch** page, which writes `data/kill_request.json`
>   (with the instant and the user, under a lock file) relative to the checkout.
> - **Live order submission is disabled** behind the shared dispatch permit, so
>   no process can place a live order right now whatever the kill state is.
>
> BEFORE GO-LIVE, one path must be decided (run/KILL relative to the state
> dir), created by the deploy step (so its directory exists), and polled every
> loop by the process that owns the dispatch permit. Until then the procedure
> below is the TARGET design, not something to rely on.

Target design: the kill switch is one file, one flag, checked every loop
(ROADMAP §13 rule 5).

```bash
touch run/KILL          # relative to engine.state_dir's parent; see above
```

It is deliberately that simple: no RPC, no socket, no authentication,
nothing that can itself be down. It would work when the UI is frozen, when the
process is unresponsive to signals, and when you are on a phone over SSH.

**What it does:** stops new order submission and flattens per the configured
policy in `oms/`.
**What it does not do:** it does not close the process, does not cancel the
broker's own resting orders placed before it existed, and does not unwind
anything the engine never booked.

The UI's kill-switch button (P11Q-05c) goes through the same confirmation and
is executed by `oms/`, never by the UI (CLAUDE.md, "The in-process decision",
point 2). **If the UI is the thing that is broken, use the file.**

---

## 2. Assess before flattening

### 2.1 What does the broker say you hold?

The broker's position book, not the engine's. Read it from the broker's own
web console if the API path is what failed. This is the authoritative number.

### 2.2 Reconcile

Run P12-04's reconciliation against the day's fills so far. The four outcomes
and what each means right now:

| Finding | Meaning at 14:30 |
|---|---|
| `Agree` | Nothing to do. |
| `ChargeMismatch` | Money is wrong, position is right. **Not an emergency.** Do not flatten for this. |
| `FillMismatch` | Quantity or price differs. The order state machine is wrong. Flatten on the BROKER's number. |
| `EngineOnly` | A phantom. The engine will hedge against something it does not hold. Correct the engine; there is nothing to close. |
| `BrokerOnly` | **The one that matters.** An open position nobody is managing. Close it manually, from the broker's console, and only then restart anything. |

### 2.3 Check the conservation ledger

`Σ(fills) + Σ(costs) + cash_delta == 0`, exactly, in paise. A breach means the
engine's own accounting is inconsistent — the numbers on the screen are not
describing the account. **When conservation is breached, no automated action is
trustworthy, including automated flattening.** Do it by hand.

---

## 3. Deciding whether to flatten at all

**Flattening is a trade, and it is usually the worst-priced trade of the day.**
This is the step that gets skipped.

Flatten when:

- The position is directional and unhedged, and you have lost the ability to
  manage it.
- The instrument is liquid enough that a market order is not a donation.
- There is time before the close.

**Do not** flatten when:

- The book is a hedged structure (a spread, a butterfly) and legging out at
  market breaks the hedge before it removes it. Closing three legs of a
  butterfly at the touch is a worse position than holding all four overnight.
  P11Q-05d measured the shape of this: a `Touch` is a bid AND an ask, and you
  buy the wings at the ask and sell the body at the bid — the round trip is not
  the mid.
- It is 15:25 and the instrument is an illiquid far-month option. Impact at that
  hour on that book can exceed a plausible overnight adverse move.
- The only problem is a charge mismatch. See §2.2.

If you hold, you have accepted an overnight risk deliberately. Write down why,
now, while you still remember the reasoning.

---

## 4. Restarting

### The restart will refuse. That is correct.

`app/warm_restart.hpp` (P12-02) checks in order: a latched breach, then
conservation, then session staleness.

- **`BreachLatched`** — the snapshot records a kill switch that had tripped, and
  it refuses to resume. **Do not work around this.** Restarting must never be a
  way to clear a halt; if it were, every operator under pressure would discover
  that rebooting makes the alarm stop.
- **`InvariantBreach`** — the snapshot's arithmetic does not hold. The state is
  evidence, not a starting point. Keep the file.
- **`StaleSession`** — the snapshot is from a different trading day. Resuming
  yesterday's positions is worse than starting flat, because the engine will
  hedge against something it does not hold. The bypass exists, is explicit, and
  is an operator decision.
- **`ChecksumMismatch`** — the file was torn or edited. Note that the file still
  *parses*; that is why parsing is not the check.

The correct restart after a breach is: resolve the underlying cause, flatten or
accept the position **manually**, remove the kill file, and start **flat** — not
from the snapshot.

### Restart order

1. Confirm the broker position is what you believe it is (§2.1).
2. Confirm no `BrokerOnly` rows remain unresolved.
3. Start in **paper mode** and let it run a full session against live data
   without placing an order. Compare its would-be fills against the market.
4. Only then remove the kill file.

Step 3 is the one that gets dropped, and it is the one that catches a
misconfiguration that would otherwise be discovered by losing money.

---

## 5. Feed loss specifically

A dead feed does not fire a threshold — P12-03 exists because of this. Every
gauge reads its last good value, every limit is satisfied, and the board goes
`Stale` on the heartbeat, not on the number.

1. **Do not trade on the last known price.** A stale book is not a book. The
   engine blocks the affected symbol (rule 9: failing loud beats trading wrong);
   confirm it actually did.
2. If positions are open and the feed is down, you are flying blind on
   valuation. That is a reason to flatten *conservatively* — hedged structures
   still should not be legged out at market.
3. Kite's WebSocket drops routinely. Sustained loss (> 60 s) is different from a
   reconnect; the `stale_after` budget in the gauge spec is what draws that line
   and it is configuration, not a constant in the code.

---

## 6. Credential compromise

The current state is not clean and this document is not the place to be
diplomatic about it: **three Kite API secrets have been exposed in a chat
transcript during development and have not been rotated.** They must be rotated
before any live capital, and that item is on the go-live checklist.

If a credential is compromised while live:

1. Kill file first (§1).
2. Invalidate the session at the broker (log out everywhere from the Kite
   console). This is faster than rotating the API secret and takes effect
   immediately.
3. Rotate the API secret at the Kite developer console.
4. Update `ALTAIR_KITE_API_SECRET`. Credentials live in environment variables
   only; `broker/` is the only directory that touches one.
5. Delete `data/kite_session.json` — it holds a live access token, is gitignored
   and must never be committed.

---

## 7. What has no procedure yet, and should

Named so they are not mistaken for solved:

- **Cross-machine failover.** Nothing exists. A hardware failure means a manual
  flatten from the broker console.
- **A partial fill during a crash.** P12-04 detects it after the fact. Nothing
  prevents it.
- **Broker-side rejection storms.** No back-pressure policy is written.
- **Rehearsal.** None of this has been performed. The first three items above
  will change once it has been.
