# P7-06 — MTBT feed qualification (specification)

> **Deliverable of `prompts/P7-06_MTBT_card.md`.** This file is the DOCUMENT, not
> the card. The card is preserved unchanged beside it.
>
> **Status: specification only, 2026-09-25.** No decoder, no transport, no socket,
> no binary was built, linked or run for this document. No licensed exchange feed
> was accessed and no network request was made. No source, test, CMake or config
> file was changed. The only files written were this document and the progress
> records the operating protocol requires (`final.md`'s execution record,
> `prompts/PROMPT_EXECUTION_QUEUE.md`, `change_by_codex.txt`); the card's "change
> no other file" is read as *no product file*, which is what it protects against.
> The recommendation in §6 is conditional, and the default branch is *do not build
> the decoder yet*.

## 0. Method, and what this document is not allowed to claim

Read on disk before writing (no live system, no feed):

- `feed/tick.hpp` — the canonical `Tick` (64 B) and `DepthUpdate` (272 B) structs
  and their pinned layout (D1–D7).
- `feed/normaliser.hpp` — the stateful half: `Verdictum`, per-instrument
  sequencing, `reset_session()`.
- `book/l2_book.hpp` — `apply()` rejects `d.seq <= b.seq`; `is_tradable()`.
- `final.md` §1, §7 (official references), §11 (latency budget), §14 (open
  decisions), §15 (reality checks).
- `prompts/audit/cx01c/G03_feed_book_findings.md` finding **C03-013** (sequence
  has no session epoch).

Every factual claim below either cites a source or is marked **UNVERIFIED** with
the specific thing that would verify it. Where the card's own wording repeats I
use it; where I cannot confirm it I say so rather than restating it as fact.

| # | Claim | Status | What verifies it |
|---|---|---|---|
| F1 | NSE sells distinct real-time data products, and exchange multicast TBT is one of them, separate from retail broker streams | **Cited** — `final.md` §7: "distinct data levels and exchange multicast TBT" | [NSE real-time data products](https://www.nseindia.com/static/market-data/real-time-data-subscription) |
| F2 | NSE documents trading-system protocols separately from data products | **Cited** — `final.md` §7 | [NSE trading protocols](https://www.nseindia.com/static/trade/platform-services-neat-trading-system-protocols) |
| F3 | A retail broker WebSocket (Kite/FYERS) delivers throttled snapshots, not the exchange's per-order/per-tick stream | **Cited** — `final.md` §1: "Retail broker streams and licensed exchange MTBT are different feeds." | Same §1 line; broker docs |
| F4 | Broker API round trip is 10–50 ms and dominates the internal microsecond budget | **Cited** — `final.md` §11, §15 | Latency table and reality checks |
| F5 | Co-location / DMA budget is unresolved | **Cited** — `final.md` §14, decision #3, status **Open** | The decision itself |
| F6 | Specific NSE TBT message semantics — per-order vs per-tick, message rates, packet layout, protocol version | **UNVERIFIED** | NSE TBT protocol document / member spec sheet, obtainable only under an agreement |
| F7 | Whether NSE TBT is available without colocation or a leased line | **UNVERIFIED** | NSE market-data subscription terms |
| F8 | BSE's equivalent TBT product, protocol requirements and access process | **UNVERIFIED** — `final.md` §7 explicitly defers this: "BSE specifics still require their own official protocol/access verification in P7-06" | BSE official protocol/access page |
| F9 | The specific update rate of Kite/FYERS WebSocket (the card's "a few updates/sec") | **UNVERIFIED** | Measured capture of the subscribed feed over a full session |
| F10 | Licensing cost, exchange fee and per-segment entitlements | **UNVERIFIED** | NSE/BSE subscription quotation |

**Reading note.** F6–F10 are exactly the facts that decide the go/no-go. They are
UNVERIFIED because this environment has no member agreement, no exchange
entitlement and no licensed feed. Marking them UNVERIFIED is the point: a
confident paragraph here would be the failure this card exists to prevent.

---

## 1. The distinction, stated once and precisely

**Exchange multicast TBT (market-by-tick).** The exchange itself publishes each
market-data event — a trade, a quote, an order-book change — as a sequenced
message on a multicast distribution, consumed from inside or adjacent to the
exchange (F1, F2). Because every event is carried in sequence and the consumer
sees the exchange's own stream, a TBT decoder can in principle reconstruct order
flow and queue position that a snapshot cannot express. The detail needed to
build against it — message identity, rates, layout, version — is F6 and is
**UNVERIFIED** here. Whether it can be received without colocation is F7 and is
also **UNVERIFIED**.

**Retail broker WebSocket (Kite/FYERS).** A broker relays throttled snapshots
over TCP to a single retail client (F3). It is a downstream, rate-limited view
of the same market. It cannot establish per-order sequencing, and its snapshots
are lossy by construction: between two snapshots the book the client holds is an
inference, not a record. The exact update rate is F9 and is **UNVERIFIED**; what
is established is that it is *throttled* and *snapshot-shaped* (F3).

**The conflation this document forbids.** Broker WebSocket data must never be
labelled MTBT, TBT, or "exchange direct" anywhere — not in a comment, a column
header, a log line or a model feature name. They differ in who publishes, in the
transport, in sequencing, and in what can be reconstructed. A feature that only
exists in TBT (queue-position models, per-order flow imbalance) is not
approximated by a broker snapshot; it is absent, and must be reported as a data
gap rather than filled with a plausible-looking number (`final.md` §15; rule 11).

---

## 2. Access prerequisites — the checklist before one line of decoder

A decoder is worth writing only when every **required** row below is settled.
"Unknown" is a valid and current state; it is not a reason to start anyway.

| # | Prerequisite | Required? | Current state (2026-09-25) | What settles it |
|---|---|---|---|---|
| A1 | NSE market-data / TBT licence held by the entity that will receive it | **Required** | **Unknown** (F10) | NSE market-data subscription quotation and agreement |
| A2 | Segment entitlements: cash (NSE/BSE), index futures/options, and each series actually decoded | **Required** | **Unknown** (F10) | Per-segment subscription record |
| A3 | BSE equivalent licence and product identification | **Required for NSE↔BSE work; optional for NSE-only** | **Unknown** (F8) | BSE official protocol/access page |
| A4 | Network reach: colocation, leased line, or whatever the licence requires | **Required** (exact form **UNVERIFIED**, F7) | **Unknown** — `final.md` §14 #3 is **Open** (F5) | NSE/BSE access requirements + a network design |
| A5 | Registered IP / hardware entitlement (MAC or NIC allow-list) | **Required on the provider's terms** | **Unknown** (F6) | Provider registration procedure |
| A6 | Receive-side hardware: a dedicated host with a TBT-capable NIC and, for kernel bypass, a DPDK-class path | **Required for the §11 ingress budget; optional for a correctness-only build** | **Not present** — `final.md` §11, "NIC → user space (busy-poll) 2 µs" is a *budget*, not a measured Altair capability | A built host, then a measured capture |
| A7 | A captured reference stream: a recorded session of the real feed, with the provider's protocol document | **Required before the decoder card** | **Not present** | The licence (A1) plus a recorded capture |
| A8 | A named owner and a data-retention decision (TBT capture is large) | Optional for correctness; required before a long capture | **Unknown** | Ops decision |
| A9 | Entitlement to *store* the feed for later replay/backtest | **Required**, if any TBT capture feeds the backtester | **Unknown** (F10) | Licence terms — redistribution/storage clauses |

**Gate.** A1 + A2 + A4(+A5) are the minimum for a single-venue NSE decoder; A3 is
additionally required for cross-venue work; A7 gates the decoder card itself.
With A1 unknown, the honest state is **no-go for construction** (§6).

---

## 3. Protocol obligations a future decoder would carry

These are requirements for a future **P7-06a decoder card**, not implementation.
Each is a thing the decoder must do or refuse; each is testable against a
captured reference stream (A7).

1. **Per-stream sequence numbers.** TBT is sequenced per stream (F6, details
   **UNVERIFIED**). The decoder must track the exchange sequence *per stream and
   per session*, detect a break, and never let a gap pass silently. Altair's own
   `Tick::seq` is explicitly **not** the exchange's (`feed/tick.hpp`, D5: "OUR
   sequence, assigned at normalisation … an exchange sequence … does not survive
   a failover"). The decoder therefore carries the exchange sequence *in addition
   to* the normaliser's, or drops it only where the contract says so — the two
   must not be confused.
2. **Session epochs.** `Normaliser::reset_session()` sets `seq_ = 0`
   (`feed/normaliser.hpp`) and `L2Book::apply()` rejects `d.seq <= b.seq`
   (`book/l2_book.hpp`), so a session rollover without clearing the book leaves
   every instrument `StaleSequence` and the book showing the previous session —
   audit finding **C03-013**. A TBT decoder must carry an explicit session/epoch
   identifier and force a book reset on epoch change. This is the same obligation
   the existing adapter already learned (FYERS adapter epochs, `source_router`).
3. **Snapshot + increment recovery.** The decoder must support a snapshot-plus-
   increments model: on gap or on start it requests/loads a snapshot, then applies
   increments from the snapshot's sequence. Until the book is synchronised it must
   publish **`NotSynchronised`**, never a partially-applied book. A book that
   looks tradable while unrecovered is the dangerous default.
4. **Gap detection and refusal.** On a detected gap the decoder must mark the
   stream unsynchronised, count it, and refuse to publish derived state until
   recovery completes. It must not interpolate across a gap. (Rule 11: failing
   loud beats trading wrong.)
5. **Duplicate suppression.** Re-sent/duplicated increments must be recognised by
   sequence and discarded without double-applying to the book.
6. **Book integrity — no fabricated tradability.** A TBT decoder must never
   surface a crossed or locked book as tradable. Cross-reference `book/` and its
   existing `is_tradable()` / crossed-book handling; a TBT stream can legitimately
   present transient crossed states during recovery, and those must be refused,
   not traded.
7. **Order-vs-tick event typing.** Where the stream distinguishes order-level from
   trade-level events (**UNVERIFIED** that it does, F6), the decoder must preserve
   the distinction. It must not synthesise an order event from a trade event or
   the reverse — that is the struct-merging mistake `feed/tick.hpp` D3 already
   rejects for touchline-vs-depth.
8. **Timestamp provenance.** Exchange time is kept as `exchange_ts` (rule 7, D2);
   receive time as `recv_ts` off the TSC clock. Plausibility gating (P0-04) stays
   in force; a TBT timestamp that fails plausibility is refused, not clamped.
9. **Bounded, allocation-free ingress.** No allocation, no unbounded queue on the
   ingress path; a full ring refuses and counts, matching the existing feed
   contracts (`lockfree/`, bounded SPSC output).

---

## 4. What Altair reuses vs. what is new

**Rule 6 governs this:** backtest and live must remain one code path. A TBT
decoder is therefore a *producer of the existing structs*, not a new pipeline.

**Reused unchanged — the boundary is `Tick` / `DepthUpdate`.**

| Reused as-is | Why it survives |
|---|---|
| `Tick` (64 B, `feed/tick.hpp`) | Canonical identity (D1), our own `seq` (D5), `exchange_ts`/`recv_ts`, integer paise, pinned layout (D7). An MTBT trade event maps onto it with no new field. |
| `DepthUpdate` (272 B) + `DepthLevel` (24 B) | `bid[]`/`ask[]` with real `bid_levels`/`ask_levels` (D6) — the "unpopulated level is zeroed" protection is exactly what a TBT book needs. |
| `Normaliser` (`feed/normaliser.hpp`) | Gating (`Verdictum`), monotonic ordering, source routing, blocked/unknown counting. Unchanged. |
| `L2Book` (`book/l2_book.hpp`) | Sequencing, `is_tradable()`, crossed/locked refusal. Unchanged, and it is the thing obligation 6 leans on. |
| `risk/cost.hpp`, strategies, `oms/paper_venue.hpp` | Nothing downstream of the normaliser changes because the wire changed. |
| TSC clock / plausibility gate (P0-03/P0-04) | Timestamps are normalised identically. |

**New, and only this.**

| New | Where | Note |
|---|---|---|
| Transport: multicast receive, membership, ring buffers | `feed/` (new file) | Bounded, allocation-free; no `oms/` include (one component, one directory). |
| Wire decoder: TBT message → `Tick`/`DepthUpdate` | `feed/` (new file) | Bounded reads, refuse-on-unknown-message, no truncation. |
| Recovery state: snapshot + increment, per-stream sequence, epoch | `feed/` (new file or extension of the adapters) | The genuinely new *state machine*; obligations 1–5. |
| A new `FeedSource` value for TBT | `instruments/contract_spec.hpp` — **not** `feed/tick.hpp` | `enum class FeedSource : std::uint8_t { Kite = 0, Xts = 1, Fyers = 2 }` with `kFeedSourceCount = 3`. That count sizes `ContractSpec::token[]` and the spec store's token index, and `Tick::source` / `DepthUpdate::source` are **persisted** by `feed/tick_store.hpp` — so a new value is a stored-meaning change with a size/count consequence, not a cosmetic label. Note the separate `altair::ui::FeedSource` in `desktop/feed_status.hpp`; the two already collide by unqualified name (`desktop/data/master_lookup.hpp:145-156` documents an out-of-range index caused by exactly that), so the value must be added carefully on both. |
| Session-epoch field on depth/book path | `feed/` + `book/` reset plumbing | Closes C03-013 for this producer too. |

**What deliberately does *not* change:** no new field on `Tick` (its layout is
pinned and replay depends on it); no second pipeline; no change to strategies,
risk or OMS; and **no new strategy is enabled by the decoder existing**. TBT is
an ingress change, not a permission to trade faster.

---

## 5. The honest performance statement

**TBT does not make Altair fast, and must not be presented as making it fast.**

- The order round trip is still bounded by the broker/exchange **gateway**, not by
  the feed. `final.md` §11: "Broker API round trip (10–50 ms via Kite/XTS REST)
  dominates all of the above." §15 agrees: "Retail API latency is the ceiling.
  10–50 ms round trips mean pure latency-arbitrage is unavailable." A TBT feed
  improves the *input*; it does not shorten the *output*.
- The §11 internal budget (~20 µs arbitrage path, ~80 µs NN path) is an
  **architecture target**, and `final.md` §11 itself says the microsecond
  architecture "buys headroom for a future co-location / DMA / FIX line … Do not
  build a strategy whose thesis requires beating a co-located HFT to a quote."
- **No latency claim may be made from this document.** Before any number is
  stated, the following must be measured on the real deployment: ingress cost
  (NIC → user space), decode cost, book-apply cost, and the end-to-end
  tick-to-decision time — each separately from the broker gateway time. "Network
  ingress: 1.0 ms (depends on co-location)" (`final.md`, latency budget) is a
  dependency, not a measurement.
- What TBT legitimately buys is **fidelity, not speed**: a recoverable, sequenced,
  non-throttled input, which is a prerequisite for any microstructure model and
  for honest replay. That is a data-quality argument, and it is the only one made
  here.

---

## 6. Go / no-go recommendation

Conditional on §2, with the default branch being the conservative one.

```
IF A1 (NSE licence) AND A2 (segments) AND A7 (captured reference stream):
        -> GO. Write P7-06a: "NSE TBT decoder + recovery state machine".
           Manifest: new feed/ files only (transport, decoder, recovery),
           plus a FeedSource enum value. Produce Tick/DepthUpdate unchanged.
           Acceptance = obligations 1-9 in §3 against the captured stream,
           including the C03-013 epoch/reset regression.

ELIF A1 known-true but A7 not yet captured:
        -> HOLD at exactly one step: "P7-06b: capture and freeze a reference
           TBT session" (ops task; no C++). The decoder card is not written
           until its fixture exists.

ELIF A3 available for BSE as well:
        -> Add P7-06c: "BSE TBT product identification + decoder", separate card,
           same obligations. Do not fold two venues into one decoder.

ELSE (A1 unknown, which is today's state):
        -> NO-GO for construction. Remain on the broker WebSocket path
           (P3-02/P3-03) for market data. Record the decision and revisit when
           §14 #3 (co-location / DMA budget) is resolved and a licence exists.
           Re-open this document when that changes; do not start a decoder from
           an assumption of access.
```

**Recommendation (today):** the **ELSE** branch. A1–A7 are all Unknown or Not
present, so there is no reference stream to decode against and no entitlement to
decode it under. Building a decoder speculatively would produce untested code for
a protocol marked **UNVERIFIED** (F6) — precisely the failure the card names.
The correct next action is an **ops/decision task**, not an engineering one.

---

## 7. Acceptance mapping (against the card's §4)

| Card acceptance | Where satisfied |
|---|---|
| 1. Broker-WS-vs-TBT distinction with citations | §1, with F1–F3 cited and F6/F7/F9 marked UNVERIFIED |
| 2. Access-prerequisite checklist, each with a state | §2, A1–A9, every row states its current state |
| 3. Decoder protocol obligations as future-card requirements | §3, obligations 1–9 |
| 4. Reuse-vs-new boundary against the normaliser, explicit | §4, with the reused table and the new table |
| 5. Conditional go/no-go with named next cards | §6 (P7-06a / P7-06b / P7-06c / ELSE) |
| 6. Unverifiable claims marked UNVERIFIED; no binary run, no other file changed | §0 table (F6–F10), and the status note at the top of this file |

---

## 8. What this document does not establish

- It does **not** establish that Altair can obtain TBT, at what cost, or on what
  network (F7, F10 **UNVERIFIED**).
- It does **not** establish the TBT wire protocol, its message set, its rates or
  its version (F6 **UNVERIFIED**).
- It does **not** establish anything about BSE's equivalent product (F8
  **UNVERIFIED**).
- It does **not** claim, and must never be cited as claiming, any latency
  improvement or HFT capability (§5).
- It is **not** a decoder. No `feed/` code was written, no binary built, no feed
  received.

**Next dependency-safe work (not this card):** the Phase 7 blockers remain
P7-05 (needs P2-10), P7-08 (needs a live feed, P3-03) and P7-09 (needs
credentials and the §14 #8 order-route decision). This document unblocks nothing
by itself; it makes the TBT question decidable and keeps the two feeds apart.



