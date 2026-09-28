# P7-06 (card) — qualify the optional MTBT feed path

> **Card vs deliverable.** This file is the CARD. The deliverable is a NEW file,
> `prompts/P7-06_MTBT_FEED_SPEC.md`. Do not overwrite this card with the output
> (that mistake was made once already — `final.md` §2.3).

## 0. Ownership and locks

Your manifest is one file: `prompts/P7-06_MTBT_FEED_SPEC.md` (create). No C++,
no CMake, no config. Other agents hold `core/types/`, `desktop/`, `oms/` and
`prompts/P6-01_ATLAS_RECONCILIATION.md` — none is your file. Everything else is
read-only to you.

## 1. Context

`final.md` §18 P7-06 and §14 open decision #11 ask whether Altair should ingest
an exchange **market-by-tick (MTBT / TBT)** feed instead of, or alongside, the
retail broker WebSocket. The danger the roadmap names repeatedly: **a broker
WebSocket is not exchange multicast TBT**, and treating one as the other
promises microstructure the data cannot support. This card produces the
qualification document that decides the question on evidence, not vocabulary.

It is a **specification only.** No decoder, no feed code — those are separate
cards, gated on licensed access this environment does not have. Your job is to
make the go/no-go decidable and to stop the two feeds being conflated.

## 2. What the document must contain

Write `prompts/P7-06_MTBT_FEED_SPEC.md` with these sections:

1. **The distinction, stated once and precisely.** What NSE/BSE multicast TBT
   is (per-order or per-tick, sequenced, UDP multicast, colocation-dependent)
   versus what a Kite/FYERS WebSocket delivers (throttled snapshots, TCP, a few
   updates/sec). Cite the official NSE data-product and trading-protocol pages
   already listed in `final.md` §18 references. Where a fact needs a source you
   cannot reach, mark it **UNVERIFIED** and say what would verify it.
2. **Access prerequisites** — the checklist that must be true before a single
   line of decoder is worth writing: licensing, colocation or a leased line,
   IP/entitlement registration, per-segment subscription, hardware/NIC. Each
   item: required / optional, and its current state (unknown is a valid state).
3. **Protocol obligations a decoder would carry**, as requirements for the
   future card, not implementation: per-stream sequence-number handling,
   snapshot-plus-increment recovery, gap detection, duplicate suppression,
   and book integrity (no crossed/locked book surfacing as tradable — cross-
   reference `book/` and the existing crossed-book handling).
4. **What Altair reuses vs. what is new.** The normaliser already emits one
   `Tick`/`DepthUpdate` (P2-01); an MTBT decoder must produce the *same* struct
   so backtest and live stay one code path (rule 6). Say what changes and what
   does not.
5. **The honest performance statement.** Even with TBT, the order round trip is
   still bounded by the broker/exchange gateway; §11 and §15 already say retail
   latency is the ceiling. The document must **not** promise HFT performance,
   and must say what measurement would be required before any latency claim.
6. **Go / no-go recommendation**, conditional on the prerequisites in §2, with
   the concrete next card named for each branch (e.g. "if licensed → P7-06a
   decoder card; if not → remain on broker WS, revisit when co-lo budget
   exists — §14 #3").

## 3. Constraints

- Every factual claim cites a source or is marked **UNVERIFIED**. A confident
  sentence about a protocol you have not seen is the failure this card exists to
  prevent.
- Never label broker WebSocket data as MTBT anywhere in the document.
- Recommend the privacy/refusal-preserving default: if access is unverified,
  the conclusion is "do not build the decoder yet", not "assume we can".
- Documents only — nothing built, linked or tested (this mirrors `ops/`).

## 4. Acceptance

1. The broker-WS-vs-TBT distinction is stated with citations.
2. The access-prerequisite checklist is present, each item with a state.
3. Decoder protocol obligations are listed as future-card requirements.
4. The reuse-vs-new boundary against the existing normaliser is explicit.
5. A conditional go/no-go with named next cards exists.
6. Every unverifiable claim is marked UNVERIFIED, and the doc says no binary was
   run and no other file was changed.

## 5. Deliverable format

The complete `prompts/P7-06_MTBT_FEED_SPEC.md`. No elisions.

```
RULES — violating any of these fails review:
1. Produce the complete document. No "...", no "rest unchanged".
2. Create only prompts/P7-06_MTBT_FEED_SPEC.md. Change no other file.
3. Do not modify any C++, CMake or config file. This card ships a document.
4. Every factual claim cites a source or is marked UNVERIFIED.
5. Never call broker WebSocket data exchange MTBT.
6. State what the evidence does not establish; do not fill gaps with plausible
   detail.
7. If a requirement is ambiguous, record both readings and say which you used.
```
