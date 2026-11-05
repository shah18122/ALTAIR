# NSE NNF and TBT: what direct exchange access would take, and what ALTAIR can use now

Status, 2026-11-05. **Draft written without the NSE PDFs.** The cloud
environment cannot reach `www.nseindia.com` or `nsearchives.nseindia.com`, so
this was written from the publicly known shape of the NSE Non-NEAT Front End
(NNF) protocol and the Tick-by-Tick (TBT) multicast feed, plus a read of
ALTAIR's own feed and order code. Every byte layout, message code and number
marked **(verify)** must be checked against the PDFs before anyone codes
against it. Source page:
<https://www.nseindia.com/static/trade/platform-services-neat-trading-system-protocols>.

Documents to read, from that page (latest versions as of Sept 2026):

| Document | What it is |
|---|---|
| `TP_FO_Trimmed_NNF_PROTOCOL_9.51` (Sept 2026; 9.50 and 9.49 July 2026) | F&O order entry over TCP. 9.51 changes one error-code text (16733); 9.49 added an error code and changed message rate control; 9.44 added immediate order acknowledgement and encryption. |
| CM Trimmed NNF protocol | The same for the cash market. |
| F&O and CM Tick-by-Tick (TBT) multicast spec | Order-level market data: every new, modify, cancel and trade. |
| Contract / security / spread master formats | Token files the other two refer to. |

## 1. Plain-words conclusions

1. **100 µs to NSE is not reachable from FYERS or from AWS.** FYERS is a REST
   and WebSocket API behind Cloudflare. Our 1.4 to 1.55 ms is mostly the wire
   and FYERS' own stack, not ALTAIR. Nothing in the NSE specs changes that.
2. **NNF is the protocol NSE members' own front-ends speak.** You cannot open an
   NNF session as a retail client. Only a trading member (or a vendor system
   the member has empanelled and the exchange has approved) gets a user ID,
   box ID and gateway connection.
3. **TBT is the order-level feed.** It is delivered by multicast inside NSE's
   colocation or over a leased line, and only to members or licensed data
   subscribers. FYERS' "TBT" socket is a 50-level depth snapshot/diff stream,
   not NSE's order-by-order TBT.
4. **What we can take now:** the discipline the TBT spec forces on a book
   builder (sequence numbers, gap detection, snapshot before diffs, recovery,
   exchange timestamps) and the NNF throttle and algo-ID rules. Our FYERS
   50-level decoder stores the sequence number but never checks it; that is the
   one concrete fix this study found (section 4).
5. **The practical path to sub-millisecond** is a broker that hosts client
   algos in its NSE colo rack, through an exchange-approved OMS (Symphony XTS,
   Greeksoft, Omnesys/NEST, ODIN, TradeLab and similar). That is a business
   decision with real costs (section 3), not a code change.

## 2. The protocols in brief (verify against the PDFs)

### NNF (order entry)

- TCP to NSE. The client first asks the **gateway router** for a gateway
  (`GR_REQUEST`, code 2400 **(verify)**), gets an IP, port and session key, then
  signs on at that gateway (`SIGN_ON_REQUEST_IN`, 2300) with user ID, password,
  broker ID, **box ID** and the protocol version NSE has approved.
- After sign-on: system information, local database update and message
  download (the day's order/trade replay) before trading.
- **Trimmed** order messages are the low-latency set: new order, modify and
  cancel (20000 / 20040 / 20070 **(verify)**) with trimmed confirmations and
  trade messages. NNF is big-endian **(verify)**; each packet has a length, a
  sequence number and (in older versions) an MD5 checksum, then a 40-byte
  message header.
- Since 9.44: immediate acknowledgement of an order before it reaches the
  matching engine, and encryption of the session (key from the gateway
  router) **(verify the cipher)**.
- **Throttle:** each user ID has a messages-per-second rate bought from NSE.
  Messages above it are rejected or held at the gateway; 9.49 changed this
  rate control **(verify how)**.
- **Algo ID:** every algorithmic order carries an exchange-approved algo ID.
  Since SEBI's retail-algo framework (circular of 4 Feb 2025, phased in from
  2025), broker APIs must also tag algo orders, require a **static IP**, and
  orders above the exchange's orders-per-second threshold (NSE: 10/s
  **(verify current)**) need a registered algo.

### TBT (market data)

- UDP multicast, several **streams** per segment, each with its own sequence
  number; A and B feeds carry the same data for arbitration **(verify)**.
- Little-endian **(verify)**. A stream header (length, stream ID, sequence
  number) then one message: **N** new order, **M** modify, **X** cancel, **T**
  trade, spread variants, and a heartbeat. Order messages carry an exchange
  timestamp (ns), order ID, token, side, price (paise) and quantity; trades
  carry both order IDs, price and quantity.
- A gap is recovered from a **TCP recovery server** by asking for a stream ID
  and a sequence range, with a cap on messages per request **(verify)**.
- The book is built by the client: there is no depth snapshot in the stream.
  A client that misses one message has a wrong book until it recovers.

## 3. What direct access would require

| Route | Who can use it | Latency to NSE | Rough cost (to confirm) |
|---|---|---|---|
| FYERS API from AWS Mumbai (today) | Anyone | 1.4–1.55 ms RTT measured, plus FYERS' processing | Free with the account |
| Broker DMA / API on a leased line or in the broker's DC | Clients of brokers that offer it, usually institutions or large prop clients | Hundreds of µs to ~1 ms | Broker-specific monthly fees, often a minimum turnover |
| **Broker colo hosting** (your server in the member's NSE colo rack, orders through the member's approved OMS/RMS) | Clients of brokers that offer it | Tens of µs to the matching engine | Rack share plus port and TBT fees; brokers quote this privately. Expect lakhs of rupees a year, not thousands. |
| Own NSE membership + colo | A SEBI-registered trading member | Lowest | Membership net worth and deposits in crores, clearing arrangement, compliance staff, NSE colo rack and TBT charges, empanelment and conformance tests for the NNF software |

What direct NNF/TBT access involves, step by step:

1. Be a trading member, or a client of one who allows it.
2. Software: write or buy an NNF front-end. Self-built software must pass NSE
   **empanelment and conformance (mock / UAT) tests**, and any algo must be
   approved and get an algo ID. Members usually buy an approved vendor system
   instead.
3. Connectivity: an NSE colo rack, or a leased line / NSE extranet link to the
   exchange. AWS is not inside NSE colo.
4. TBT licence and multicast ports in colo; recovery server access.
5. RMS: the member's pre-trade risk system sits in the path and adds its own
   microseconds.

## 4. What ALTAIR can use now, through FYERS

Checked in the code on 2026-11-05.

1. **Sequence-gap detection on the FYERS 50-level book.**
   `feed/fyers_tbt.hpp` stores `sequence_no` per ticker (line ~331) but never
   compares it with the last one, and nothing outside that file reads
   `seen_snapshot`. A dropped diff therefore leaves a silently wrong book, and
   a diff that arrives before the first snapshot builds a partial book that is
   published. TBT practice: per ticker, publish nothing until a snapshot has
   been seen; when `sequence != last + 1` mark the book stale, stop publishing
   it, and resubscribe to get a fresh snapshot. First check from recorded
   frames whether FYERS numbers diffs contiguously per ticker.
2. **Exchange time, not receive time.** `app/price_service_main.cpp` sets
   `exchange_ts_ns` on the 50-level book to the local receive time; the decoder
   already has FYERS' `feed_time`. Carry both, as TBT carries the exchange
   timestamp, so feed lag can be measured per message.
3. **Crossed or locked book check** after each applied diff (best bid ≥ best
   ask means a missed update): treat it like a gap.
4. **Order throttle:** `oms/throttle.hpp` already checks per-second, per-minute
   and per-day windows and refuses rather than queues, which matches NNF
   practice. Load FYERS' published limits and the SEBI 10 orders/s retail-algo
   threshold into it, and put an algo tag on every order (`orderTag` is
   already supported in `oms/fyers_adapter.hpp`).
5. **Static IP:** the SEBI framework requires a whitelisted static IP for API
   orders. The Elastic IP plan in the latency work covers this; register that
   IP with FYERS.
6. **Order-level book code is ready for a real TBT feed.**
   `book/queue_position.hpp` already handles sequenced order events. If a
   broker later provides NSE TBT, the decoder is a small fixed-layout parser in
   the style of `feed/fyers_tbt.hpp`.

Items 1 to 3 are done (2026-11-05): `feed/fyers_tbt.hpp` gates on the snapshot,
marks a book stale on a sequence gap or a crossed book, and the price service
resubscribes that ticker for a fresh snapshot; the 50-level book carries FYERS'
feed time. If FYERS numbers diffs across symbols, the sequence check switches
itself off and the service prints that once.

## 5. Practical path

1. Now: the FYERS fixes above. They protect correctness; they do not cut the
   1.5 ms.
2. Next: ask brokers, in writing, three questions: do you host client servers
   in your NSE colo rack; which approved OMS do clients reach NNF through
   (and is there an API to it); is NSE TBT available on that rack, and what is
   the all-in yearly cost. Brokers known to run colo and offer DMA or API
   access to larger clients include the large full-service houses (Kotak,
   Motilal Oswal, Nuvama, IIFL) and API-focused brokers on Symphony XTS
   (for example Tradejini). **Confirm with each; offers change.**
3. Only if a strategy shows an edge that needs sub-millisecond speed after
   costs should this move beyond enquiries. No strategy has that today
   (see `docs/STATUS.md`).
