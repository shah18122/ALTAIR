# P6-06 — Data-dependent model gates

These are hard gates, not suggestions. A model card may run on a labelled
synthetic numerical fixture, but Atlas and its result view must still say
`NOT MARKET VALIDATED` until the corresponding evidence gate passes.

| Gate | Blocks | Evidence required | Exact next task | Owner/status |
|---|---|---|---|---|
| G-DATA-01 order-level depth | M20 queue, M21 fill, M22 Hawkes | licensed sequenced order-by-order stream, recovery snapshot, gap/duplicate counters | capture a permitted reference session and validate it against `P7-06_MTBT_FEED_SPEC.md` before writing a decoder | external licence; blocked |
| G-DATA-02 labelled fills | M21 fill probability | timestamped own orders, queue state and terminal outcome without survivorship filtering | define redacted immutable fill-label schema and collect paper/live-authorised labels | paper possible after P7-08; live external |
| G-DATA-03 point-in-time fundamentals | M18/M19 factors | release timestamps, restatements, delistings and universe membership as known then | license a PIT source and add an as-of join acceptance fixture | external dataset; blocked |
| G-DATA-04 event/news corpus | M23 | licensed text, publication timestamp, corrections and entity mapping | select corpus/licence and audit timestamp semantics before ingestion | external dataset; blocked |
| G-DATA-05 RL environment | M24–M27 | action/state/reward contract, costs, latency, partial fills, conservation and heuristic baseline | complete P7-08 paper-session integration, freeze environment v1, publish baseline | engineering after P7-08 |
| G-DATA-06 seasonal history | M06 | enough observations for each claimed season and evaluation fold | inventory each symbol/interval and refuse under-covered seasonal fits | local data audit; open |
| G-DATA-07 option validation surface | M09–M11 | contracts, timestamps, rates/dividends and executable quotes | add dated option-chain fixtures; analytical synthetic tests remain numerical only | external/live data; blocked |
| G-DATA-08 neural backend | M03/M12–M16/M25–M27 | deterministic CPU baseline; MPS is optional acceleration only | run P1-05 on Apple Silicon before claiming GPU benefit | Apple hardware; blocked |

Required UI behavior:

- Detail pages show the gate ID, current state and evidence location.
- Missing data refuses a run; it never substitutes random or forward-filled data.
- A synthetic fixture is labelled `NUMERICAL FIXTURE`, never paper/live evidence.
- “All models complete” is forbidden while any M01–M27 child remains open.
