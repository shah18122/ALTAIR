# CX-01 — FYERS backup: prerequisite assessment

Reader: claude-lead · Date: 2026-09-14 · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`
Status: **RESEARCH ONLY.** No FYERS account, app, credential, socket or order was touched.
Kite remains the assumed primary; FYERS is the requested backup.

## 1. Source quality — read this first

| Source | Fetched 2026-09-14 | Treated as |
|---|---|---|
| `https://myapi.fyers.in/docsv3` (the requested official docs) | **HTTP 404** to the fetch tool; `https://api-docs.fyers.in/` 307-redirects to `https://myapi.fyers.in/` (a JS app) | **Not read.** Every fact below still needs checking against it in a browser before implementation |
| FYERS support knowledge base (`support.fyers.in`) | 3 articles read | Official |
| `github.com/FyersDev/fyers-skills` `references/{auth,websocket,symbols,market-data,rate-limits}.md` | read | Official org, but agent-skill documentation, not the API reference — **secondary-official** |
| PyPI `fyers-apiv3` 3.1.17 (released 2026-09-06, MIT) | project page read | Official SDK description |
| FYERS community forum, search-engine summaries | snippets only | **Unverified** — used only where they agree with the above |

A fact marked **UNVERIFIED** must not become a constant, a limit or a parser rule
without a recorded fixture or a primary-document citation.

## 2. What the sources say

### 2.1 Authentication (fyers-skills `auth.md`; SEBI article)
- `GET https://api-t1.fyers.in/api/v3/generate-authcode?client_id=<APP_ID>&redirect_uri=<URI>&response_type=code&state=<random>` → browser login → redirect with `auth_code`.
- `POST https://api-t1.fyers.in/api/v3/validate-authcode` with JSON `{grant_type:"authorization_code", appIdHash, code}`; `appIdHash = SHA-256 hex of "<app_id>:<secret>"`. Returns `access_token` and `refresh_token`.
- Access token expires at the end of the trading day. A 15-day refresh token + PIN flow exists (`/api/v3/validate-refresh-token`), **but** the FYERS SEBI-2026 article states "Continuous refresh-token sessions will not be supported" and "Complete 2FA once every trading day". Treat daily interactive login as the only supported path.
- Request header: `Authorization: <app_id>:<access_token>`.
- Comparison: Kite's checksum is SHA-256 of `api_key + request_token + api_secret` (no separator). `broker/sha256.hpp` is reusable; the input construction is not.

### 2.2 SEBI retail-algo rules via FYERS, from 2026-04-01 (support article)
- Orders accepted **only** from a registered App ID mapped to a **whitelisted static IP**.
- A new API app must be activated/configured.
- Daily 2FA; no continuous refresh-token sessions.
- **Maximum 10 orders per second.**
- **Market orders are converted to MPP (market price protection) orders.**
- The article does not say whether data-only API use is exempt from the static-IP rule — **UNVERIFIED**.

### 2.3 REST limits (fyers-skills `rate-limits.md`; forum agrees)
- 10 requests/s, 200/min, 100,000/day; order place+modify+cancel combined ≤10/s.
- Breaching the per-minute cap more than 3 times in a day blocks the user for the rest of the day.
- HTTP 429 / code `-429`; order responses carry `Retry-After` and `X-Retry-After-Ms`.

### 2.4 Market data REST (fyers-skills `market-data.md`)
- `/data/quotes`: ≤50 symbols per request. `/data/depth`: **5 levels** with price, volume and order count.
- `/data/history`: seconds (5S–45S, last 30 trading days), minutes 1–240 (≤100 days per request), D/W/M (≤366 days per request); data from 2017-07-03; OI via `oi_flag`.
- `/data/options-chain-v3`: ≤50 strikes; Greeks optional.
- **Prices in rupees, timestamps in epoch seconds.**

### 2.5 WebSockets
| Socket | Endpoint | Wire format | Limits |
|---|---|---|---|
| Data (LTP/quote/5-level depth, lite mode) | SDK-managed; not stated | **Not documented for non-SDK clients.** The support article states "Direct details on the WebSocket message or packet structure for API v3 are not provided" and points to the Python/Node SDKs | **CONFLICT:** 5,000 symbols/connection (fyers-skills, forum) vs "tracking of 200 symbols" (PyPI description) — **UNVERIFIED** |
| Order/trade/position updates | `wss://socket.fyers.in/trade/v3` | JSON; subscribe `{"T":"SUB_ORD","SLIST":[...],"SUB_T":1}`; send `"ping"` ~every 10 s | not stated |
| TBT (tick-by-tick, 50-level depth) | `wss://rtsocket-api.fyers.in/versova` | Protocol Buffers, public schema `https://public.fyers.in/tbtproto/1.0.0/msg.proto`; snapshot then diffs | NFO + NSE equity only; 3 connections/user; **5 symbols/connection** |

Data and order updates **cannot share one socket** (support article); each is a separate connection with its own health.

### 2.6 Symbols and master files (fyers-skills `symbols.md`)
- Formats: `NSE:SBIN-EQ`, `NSE:NIFTY50-INDEX`, futures `NSE:NIFTY20OCTFUT`, monthly option `NSE:NIFTY20OCT11000CE`, weekly option `NSE:NIFTY20D1025000CE` (single-letter month code). (The skills file spells the bank index `NIFYBANK`; verify against the master — possibly a typo.)
- Masters at `https://public.fyers.in/sym_details/` (e.g. `NSE_FO_sym_master.json`, CSV also), refreshed daily. Fields include `symTicker`, `minLotSize`, `tickSize`, `expiryDate` (epoch s), `strikePrice`, `optType`, `fyToken`.
- `fyToken` layout: exchange(2) · segment(2) · expiry(6, YYMMDD) · exchange token(2–6).

## 3. What exists in Altair today (verified by grep, 2026-09-14)

- **No FYERS code or config anywhere** in first-party directories.
- `instruments/contract_spec.hpp:38-39`: `enum class FeedSource : std::uint8_t { Kite = 0, Xts = 1 }; kFeedSourceCount = 2`. It sizes `ContractSpec::token[kFeedSourceCount]` (`:79`) and the spec store's `token_index_[kFeedSourceCount][kTokenIndexCap]` (`:428`).
- `feed/failover.hpp:78-200`: per-source health arrays sized by `kFeedSourceCount`.
- `feed/tick.hpp:101,129`: `Tick::source` and `DepthUpdate::source` are `FeedSource` fields at fixed offsets 56 and 264 (`static_assert`s `:161,165`). `feed/tick_store.hpp` persists ticks (65 B/tick per LEDGER P2-06), so **value 1 (`Xts`) is a stored meaning.**
- `oms/throttle.hpp:212`: `enum class Venue : std::uint8_t { None = 0, Kite, Xts }`; `Venue::Xts` is refused (LEDGER P4-07), not rerouted.
- `desktop/feed_status.hpp:64`: a **second, unrelated** `altair::ui::FeedSource {Unspecified, Replay, Kite, Xts}`. `desktop/data/master_lookup.hpp:141-157` records that confusing the two enums already caused an out-of-bounds token read once (P11Q-11), now guarded by a `static_assert`.
- Reusable transport: `broker/https_client.hpp` (Beast + OpenSSL, verified TLS) and `broker/kite_ticker.hpp` (Beast WebSocket). Their suitability — timeouts, body limits, reconnect bounds — is under review in group G14 and is not assumed here.
- **Adding the enum value alone compiles but misbehaves** (reviewer G04, C04-005). The reconciler merges only the Kite and XTS token slots, so a FYERS-only contract would be blocked as "no broker". The failover watchdog can choose only between Kite and XTS. Any identity-migration card must change the reconciler merge and the failover source set, not just the enum and the array sizes. Whether FYERS exchange tokens fit the 32-bit `token[]` slot is **UNVERIFIED**.
- There is **no assembled execution path for Kite either**: no application drains `oms::drain_intents`, and `oms/kite_adapter.hpp` builds request bodies without a transport (LEDGER P4-05; Codex memory; revalidated by G13/G15). An execution backup needs a primary that executes.

## 4. Prerequisites, dependency-ordered

### 4.1 User / account (blocking, owned by Smit)
1. FYERS account with API access; create and **activate** an API app.
2. **Static IP** for order placement. A home connection is usually dynamic, so this implies a static-IP line or a hosted machine — it interacts with ROADMAP open decision #1 (prod OS / host).
3. Confirm the market-data entitlement and any fee for API data and TBT — **not found** in the sources read.
4. Confirm whether data-only use needs the static IP.
5. Daily interactive login with 2FA, performed by the user. Altair must not automate 2FA or store a password/TOTP.
6. Rotate the exposed Kite key/secret first (outstanding since P29), so two brokers are not exposed at once.

### 4.2 Engineering, market-data backup (after CX-02/CX-03 contracts exist)
1. **Identity migration card.** Add `FeedSource::Fyers = 2` and `kFeedSourceCount = 3`. Never reuse or rename `Xts = 1`. Bump the tick-store format version, or prove old files still read with the same meaning. Update both layout `static_assert`s, every array sized by the count, and `Venue`. Decide whether `altair::ui::FeedSource` gains a separate `Fyers` value, and keep the P11Q-11 `static_assert` discipline.
2. **Symbol master parser → Reconciler**, as a cross-check source. Map through the *exchange token* inside `fyToken` against the NSE/Kite-reconciled `InstrumentId`, never by symbol string. Refuse what does not reconcile (hard rules 1 and 9).
3. **Exact unit conversion**: rupee decimal strings to integer paise by string arithmetic, mirroring `kite::kite_paise_to_rupees` in reverse. No `double` on the path (rule 3). Epoch seconds become `Timestamp` through the P0-04 per-source time spec.
4. **Data-socket feasibility spike, before any decoder card.** The wire format is undocumented. Options, in preference order:
   (a) derive the frame layout from the MIT-licensed official SDK source, record real frames as versioned fixtures, and check FYERS terms of use for non-SDK clients;
   (b) TBT protobuf, which is documented but limited to 5 symbols per connection and 3 connections — enough for a handful of futures, not a 168-token chain;
   (c) a Python SDK sidecar, which **violates "no Python in the runtime"** and needs an explicit user decision;
   (d) REST polling (10/s, ≤50 symbols) is **not** a substitute for a fresh depth stream and may feed only display or stale-labelled state.
5. **Failover policy (data only)**: health is keyed on `recv_ts`; switching sources is recorded; recovery is deterministic. Historical bars or REST snapshots never become tradable state.

### 4.3 Engineering, execution backup (only after a live Kite OMS loop exists)
1. Order-socket JSON decoder against recorded fixtures, with an explicit "uncertain" order state.
2. FYERS OMS adapter behind the existing throttle: 10 orders/s combined is a hard ceiling. Market orders become MPP, so Altair's order types must map explicitly rather than assuming market semantics.
3. **Routing rule: an uncertain Kite submission is reconciled at Kite. It is never retried at FYERS.** Positions and margin stay tied to their account; switching requires reconciled state, available margin, an authorised policy and fresh pre-trade risk checks.
4. Contract-note reconciliation for a second broker (P12-04 path) and a FYERS charge schedule in `charges.toml`, effective-dated and marked UNVERIFIED until checked.

## 5. Verdict

**FYERS is not implementation-ready**, and the blockers are mostly not code.

| Capability | Blocked on |
|---|---|
| Market-data backup | undocumented data-socket wire format (spike 4.2.4); identity/persistence migration (4.2.1); account and entitlement (4.1) |
| Execution backup | static IP + app activation (4.1); **no assembled Kite execution loop to back up**; OMS uncertain-state design |

Recommended cards appear in `CX01C_PHASE2_TASKS.md` (FY-series), all after the CX-02 correctness cards.

## Sources
- [FYERS API v3 docs (requested; 404 to fetch tool)](https://myapi.fyers.in/docsv3)
- [FYERS support: WebSocket message structure](https://support.fyers.in/portal/en/kb/articles/can-you-provide-details-about-the-websocket-message-structure-in-fyers-api-v3)
- [FYERS support: SEBI retail algo rules from 2026-04-01](https://support.fyers.in/portal/en/kb/articles/what-are-the-new-sebi-rules-for-retail-algo-trading-from-april-01-2026)
- [FYERS support: order updates and market data on separate sockets](https://support.fyers.in/portal/en/kb/articles/can-i-subscribe-to-both-order-updates-and-market-data-simultaneously-through-websocket)
- [FYERS support: symbol master CSV](https://support.fyers.in/portal/en/kb/articles/where-can-i-find-the-symbol-master-csv-file-for-reference-15-11-2023)
- [FyersDev/fyers-skills](https://github.com/FyersDev/fyers-skills) — `references/auth.md`, `websocket.md`, `symbols.md`, `market-data.md`, `rate-limits.md`
- [FyersDev/fyers-api-sample-code](https://github.com/FyersDev/fyers-api-sample-code)
- [PyPI fyers-apiv3](https://pypi.org/project/fyers-apiv3/)
- [FYERS community: Rate Limit v3](https://fyers.in/community/t/rate-limit-v3/13127) (secondary)
