# P2-02 — `feed/kite_decoder`: the Kite binary tick frame → `Tick` / `DepthUpdate`

> Phase 2 · Card 2 of 9 · Status: **DONE** 2026-08-31
> Depends on: P2-01 (`feed/tick.hpp`) · P1-01 (`SpecStore`) · P0-02, P0-04
> Feeds: P2-04 (normaliser), P2-05 (failover), P2-06 (tick store)
>
> **Architect's note.** No network, no credentials, no data. This card turns a
> byte buffer into structs; whoever hands it the bytes is P2-04's problem. A
> Phase 2 decoder that opens a socket is one that needs credentials it should
> not have, which is the same line P1-04 drew.
>
> The wire format is **not guessed**. Every offset below is read from Zerodha's
> own client, `research/reference/gokiteconnect/ticker/ticker.go`, functions
> `splitPackets`, `parsePacket` and `convertPrice`. It is the one Phase 2 card
> whose schema was never in question, which is why it can be built before a
> single live byte arrives.
>
> **This is `ALTAIR_HOT`.** It runs on every tick.

---

## 1. THE WIRE FORMAT

Big-endian throughout. One WebSocket binary message is a frame:

```
[u16 packet_count] then, repeated: [u16 packet_len][packet_len bytes]
```

A message shorter than 2 bytes is a **heartbeat**, not an error — Kite sends
1-byte keepalives.

The mode is inferred from the packet length. There is no mode field:

| Length | Mode | Notes |
|---:|---|---|
| 8 | LTP | token + last price only |
| 28 | index quote | indices carry no volume and no depth |
| 32 | index full | adds an exchange timestamp |
| 44 | quote | tradable, no depth |
| 184 | full | adds OI, timestamp, and 5+5 depth |

Field offsets, quote/full (all `u32` unless noted):

```
 0  token         8  last_qty      16  volume        24  total_sell
 4  last_price   12  avg_price     20  total_buy     28  open
32  high         36  low           40  close
--- full only ---
44  last_trade_time   48  oi   52  oi_day_high   56  oi_day_low   60  timestamp
64  buy depth  [5 x 12: u32 qty, u32 price, u16 orders, u16 pad]
124 sell depth [5 x 12: same]
```

Index packets are laid out differently — `4` last, `8` high, `12` low, `16`
open, `20` close, and `28` timestamp on the 32-byte form.

**The segment is the low byte of the token**, and it selects the price
divisor. `Indices` is segment 9, which is why `256265` is NIFTY 50.

---

## 2. THE SEVEN DECISIONS

**D1 — Bytes in, structs out. Nothing else.**
No socket, no credentials, no clock. `recv_ts` is a parameter, so the decoder
is deterministic and a replay of the same bytes produces the same structs
(rule 6, rule 10).

**D2 — An unknown token is skipped and counted, not an error.**
A subscription confirmation can arrive before the spec store has the
instrument, and one unmappable packet must not discard the other forty in the
frame. It is counted so the count can be alarmed on; a decoder that silently
drops is a decoder nobody notices failing.

**D3 — A blocked instrument is skipped and counted separately.**
It already resolved, and P1-06 already decided not to trade it. Decoding it
into the pipeline would put a symbol the reconciler blocked in front of the
strategies.

**D4 — Currency derivatives are SKIPPED AND COUNTED, never truncated.**
*(Amended during review — the card first said "returns `UnsupportedScale`".)*
`convertPrice` divides by 10⁷ for `NseCD` and 10⁴ for `BseCD`. An NSE-CD wire
integer is in units of 10⁻⁷ rupees — **five decimal places finer than a
paisa** — so storing it in `Price` silently loses them. This is the P0-01
carried-debt row arriving for real. The decoder never produces the value.

But it does **not** abort the frame. Aborting would mean a single CD instrument
in the subscription kills every frame it appears in: the feed goes dark and
looks like a decoder bug when the real fault is one line of universe config.
Skipped, counted in `unsupported_scale`, and the surrounding packets survive.
A non-zero counter is loud enough. Rule 9 is satisfied by never emitting the
wrong number, not by taking the feed down with it.

**D5 — Kite exchange timestamps are ONE-SECOND resolution, and two modes have
none at all.**
`b[60:64]` is a Unix time in **seconds**. Sub-second exchange latency is
therefore not measurable from Kite at all — only `recv_ts` has that
resolution, and it measures our receipt, not the exchange. LTP and quote modes
carry no timestamp whatsoever: those get `exchange_ts = recv_ts` plus a new
`TickFlag::NoExchangeTs`, so ordering still works and anything doing latency
arithmetic knows to exclude them. Setting `Timestamp::epoch()` instead would
put 1970 into the pipeline; leaving it zero would be worse.

**D6 — `bid_levels` counts leading levels with a non-zero price.**
Kite always sends five slots and zero-pads the unused ones. A zeroed level is
not a level: a bid at price 0 with the count claiming it is real is infinite
edge. Counting stops at the first zero price.

**D7 — A malformed packet length skips that packet; a length that runs past
the buffer aborts the frame.**
The two are different. An unrecognised-but-consistent length leaves the
framing intact, so the remaining packets are still readable. A length that
overruns means the framing itself is lost and everything after it is garbage.

---

## 3. FILE MANIFEST

```
CREATE   feed/kite_decoder.hpp
CREATE   feed/tests/test_kite_decoder.cpp
MODIFY   feed/tick.hpp          (D5: add TickFlag::NoExchangeTs)
MODIFY   feed/CMakeLists.txt
```

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <feed/tick.hpp>
#include <instruments/contract_spec.hpp>

namespace altair {

enum class KiteDecodeError : std::uint8_t {
    ShortFrame,        // a packet length runs past the buffer — framing lost
    OutputFull         // the caller's arrays cannot hold this frame
};

struct KiteDecodeResult {
    std::size_t ticks;            // Tick structs written
    std::size_t depths;           // DepthUpdate structs written
    std::size_t unknown_token;    // D2 — skipped, and worth alarming on
    std::size_t blocked;          // D3 — skipped, already refused by P1-06
    std::size_t bad_length;       // D7 — unrecognised mode, framing intact
    std::size_t unsupported_scale;// D4 — currency derivative, skipped
};

/// Kite segment, the low byte of the instrument token.
enum class KiteSegment : std::uint8_t {
    NseCM = 1, NseFO = 2, NseCD = 3, BseCM = 4, BseFO = 5,
    BseCD = 6, McxFO = 7, McxSX = 8, Indices = 9
};

[[nodiscard]] constexpr KiteSegment kite_segment_of(std::uint32_t token) noexcept;

/// Wire integer units per rupee, per gokiteconnect's convertPrice.
/// 10'000'000 for NseCD and 10'000 for BseCD — both FINER than a paisa.
[[nodiscard]] constexpr std::int64_t kite_price_scale(KiteSegment) noexcept;

/// Decode one WebSocket binary message. ALTAIR_HOT. Allocates nothing.
///
/// `seq` is advanced once per emitted struct and is the caller's session
/// counter (rule 10). `recv_ts` is a parameter — this reads no clock.
/// A buffer shorter than 2 bytes is a heartbeat: zero packets, not an error.
[[nodiscard]] ALTAIR_HOT std::expected<KiteDecodeResult, KiteDecodeError>
decode_kite_frame(const std::uint8_t* buf, std::size_t len,
                  const SpecStore& store, Timestamp recv_ts,
                  std::uint32_t& seq,
                  Tick* ticks, std::size_t tick_cap,
                  DepthUpdate* depths, std::size_t depth_cap) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. Every multi-byte read is big-endian, assembled byte by byte. No
   `reinterpret_cast` onto the buffer: the payload is unaligned and casting a
   `uint32_t*` onto it is UB, not merely slow.
2. Every read is bounds-checked against `len` **before** it happens.
3. `len < 2` yields an empty result, not an error (heartbeat).
4. Prices convert by the segment scale. Scale != 100 returns
   `UnsupportedScale` (D4). No `double` anywhere in the conversion.
5. A `full` packet emits **both** a `Tick` and a `DepthUpdate`; quote and LTP
   emit only a `Tick`. Index packets never emit a `DepthUpdate`.
6. `bid_levels`/`ask_levels` count leading non-zero-price levels (D6).
7. Capacity is checked **once, up front**. A frame of N packets yields at most
   N ticks and N depth updates, and N is the first field of the frame, so
   insufficient capacity returns `OutputFull` with **nothing written and `seq`
   untouched**. A mid-stream check would abort after partially filling the
   caller's arrays and return an error carrying no count — every tick already
   decoded would be lost with no way to learn how many there were.
8. No allocation, no exceptions, no `double`, no clock read.

---

## 6. ACCEPTANCE TESTS

1. **`frame_splitting`** — a frame with three packets of different modes yields
   three results; a 1-byte heartbeat yields zero and no error; a 0-byte buffer
   likewise; a packet length that overruns the buffer is `ShortFrame`.
2. **`ltp_mode`** — an 8-byte packet gives a `Tick` with the right price and
   `NoExchangeTs` set; no `DepthUpdate` is emitted.
3. **`full_mode_prices_and_oi`** — a 184-byte packet decodes last price,
   last qty, volume, OI and the exchange timestamp; assert the timestamp is
   **second-resolution** by showing the low nanoseconds are zero.
4. **`depth_levels_and_padding`** — five populated levels give
   `bid_levels == 5`; two populated then zeros give 2; a fully zeroed side
   gives 0 and `best_bid` returns nullptr. Assert the zeroed slot's raw price
   is 0, so the test shows what D6 prevents.
5. **`currency_derivative_is_refused`** — a CD packet **between two good ones**
   is counted in `unsupported_scale` and never decoded, while both neighbours
   survive. Assert the CD price WOULD have been wrong by 10⁵ if truncated.
6. **`unknown_and_blocked_tokens`** — a token absent from the store counts in
   `unknown_token`; a blocked one counts in `blocked`; neither aborts the
   frame, and a good packet after both still decodes.
7. **`index_packets`** — 28- and 32-byte index packets decode a price and emit
   no depth; the 32-byte form carries a timestamp and the 28-byte form sets
   `NoExchangeTs`.
8. **`big_endian_and_alignment`** — decode the same packet from an
   **odd-aligned** buffer offset and get identical output, proving no
   misaligned word load; and assert a known byte pattern decodes to the
   big-endian value, not the little-endian one.

---

## REVIEW RECORD — P2-02

Reviewed 2026-08-31. MSVC 19.51.36256, `/std:c++latest /W4 /permissive- /O2`.

| Gate | Verdict | Evidence |
|---|---|---|
| 1 compiles clean | PASS | zero warnings. |
| 2 contract honoured | PASS, **card amended twice** | D4 and requirement 7, both below. |
| 3 manifest respected | PASS | `feed/kite_decoder.hpp`, `feed/tests/test_kite_decoder.cpp`, `feed/tick.hpp` (the D5 flag, declared in the manifest), `feed/CMakeLists.txt`. |
| 4 tests pass | PASS | 8 named tests + capacity + benchmark, 71 checks. |
| 5 no hot-path allocation | PASS | `ALTAIR_HOT`. No `new`/`malloc`/`vector`/`string`/`function`. Output arrays are the caller's. |
| 6 latency budget | **PASS, measured** | **70 ns per full packet, 703 ns per 10-packet frame** against ROADMAP §11's 3 µs. Batch-timed over 200'000 iterations, with a sink asserted non-zero so the loop cannot be optimised away (the P0-03 lesson). |
| 7 numerical / financial | PASS | no floating point anywhere. At scale 100 the wire value IS paise, so there is no division at all. Currency derivatives are never decoded. |
| 8 physics | PASS | Kite's exchange timestamp is **one-second resolution**; the test asserts the sub-second digits are zero. Sub-second exchange latency is therefore not measurable from this feed — only `recv_ts` has that resolution, and it measures our receipt. Recorded as a Phase 2 finding. |

### Two defects review found, both in decisions rather than code

1. **A currency derivative aborted the whole frame.** The implementation
   matched D4 as written, and D4 was wrong. One CD instrument in the
   subscription would have killed every frame containing it — the feed goes
   dark, looks like a decoder bug, and the real fault is one line of universe
   config. Now skipped and counted, with the surrounding packets surviving.
2. **`OutputFull` fired mid-stream**, after partially filling the caller's
   arrays, and `std::expected` cannot carry a count alongside an error — so
   every tick already decoded was lost with no way to know how many. The packet
   count is the frame's first field, so the check moved up front and became
   all-or-nothing. The test asserts nothing was written and `seq` is untouched.

### And one in the test harness

The benchmark **segfaulted**. `Buf` held 1024 bytes; ten full-mode packets in
one frame is 2 + 10 x (2 + 184) = **1862**. The harness walked off the end of
its own array. It crashed rather than corrupting quietly, which was luck.
Capacity raised to 8192 and both `u8` and `frame` now bounds-check and report
rather than overflow — a test harness that can corrupt memory can also
manufacture a passing result.
