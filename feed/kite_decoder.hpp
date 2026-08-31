// feed/kite_decoder.hpp — Kite's binary tick frame → Tick / DepthUpdate.
//
// P2-02. Bytes in, structs out. No socket, no credentials, no clock: whoever
// hands this a buffer is P2-04's problem, and `recv_ts` is a parameter so the
// same bytes always decode to the same structs (rule 6, rule 10).
//
// The wire format is NOT guessed. Every offset here is read from Zerodha's own
// client, research/reference/gokiteconnect/ticker/ticker.go — splitPackets,
// parsePacket and convertPrice. The layout is documented in
// prompts/P2-02_kite_decoder.md §1.
//
// This is ALTAIR_HOT. It runs on every tick.

#pragma once

#include <feed/tick.hpp>   // brings core/types/units.hpp, which defines ALTAIR_HOT
#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class KiteDecodeError : std::uint8_t {
    ShortFrame,        // a packet length runs past the buffer — framing is lost
    OutputFull         // the caller's arrays cannot hold this frame — see below
};

struct KiteDecodeResult {
    std::size_t ticks = 0;          // Tick structs written
    std::size_t depths = 0;         // DepthUpdate structs written
    std::size_t unknown_token = 0;  // D2 — skipped, and worth alarming on
    std::size_t blocked = 0;        // D3 — skipped, already refused by P1-06
    std::size_t bad_length = 0;     // D7 — unrecognised mode, framing intact

    /// D4 — a currency derivative, skipped because Price cannot hold it.
    ///
    /// SKIPPED and counted, not an error. Aborting the frame would mean a
    /// single CD instrument in the subscription kills every frame it appears
    /// in: the feed goes dark and looks like a decoder bug, when the real
    /// problem is one wrong line in the universe config. A non-zero counter
    /// here is loud enough, and it costs no good ticks.
    std::size_t unsupported_scale = 0;
};

/// Kite segment — the LOW BYTE of the instrument token.
/// Indices is 9, which is why 256265 (NIFTY 50) has segment 9.
enum class KiteSegment : std::uint8_t {
    NseCM = 1, NseFO = 2, NseCD = 3, BseCM = 4, BseFO = 5,
    BseCD = 6, McxFO = 7, McxSX = 8, Indices = 9
};

[[nodiscard]] constexpr KiteSegment kite_segment_of(std::uint32_t token) noexcept {
    return static_cast<KiteSegment>(token & 0xFFu);
}

/// Wire integer units per RUPEE, per gokiteconnect's convertPrice.
///
/// 100 everywhere except currency derivatives, where the wire is 10'000'000
/// (NSE) or 10'000 (BSE) units per rupee — both FINER than a paisa, so a
/// `Price` cannot hold them without loss. See D4.
[[nodiscard]] constexpr std::int64_t kite_price_scale(KiteSegment s) noexcept {
    switch (s) {
        case KiteSegment::NseCD: return 10'000'000;
        case KiteSegment::BseCD: return 10'000;
        default:                 return 100;
    }
}

namespace detail {

/// Packet lengths, which is how Kite encodes the mode. There is no mode field.
inline constexpr std::size_t kKiteLtpLen        = 8;
inline constexpr std::size_t kKiteIndexQuoteLen = 28;
inline constexpr std::size_t kKiteIndexFullLen  = 32;
inline constexpr std::size_t kKiteQuoteLen      = 44;
inline constexpr std::size_t kKiteFullLen       = 184;

inline constexpr std::size_t kKiteBuyDepthOff  = 64;
inline constexpr std::size_t kKiteSellDepthOff = 124;
inline constexpr std::size_t kKiteDepthEntry   = 12;   // u32 qty, u32 px, u16 orders, u16 pad

/// Big-endian u16/u32, assembled byte by byte.
///
/// NOT a reinterpret_cast onto the buffer: the payload sits at an arbitrary
/// offset inside the WebSocket message, so a `uint32_t*` cast onto it is a
/// misaligned load — UB, not merely slow — and would also be little-endian on
/// x86, which is the wrong answer twice.
[[nodiscard]] ALTAIR_HOT constexpr std::uint16_t
be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8)
                                      | static_cast<std::uint16_t>(p[1]));
}

[[nodiscard]] ALTAIR_HOT constexpr std::uint32_t
be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24)
         | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)
         |  static_cast<std::uint32_t>(p[3]);
}

/// Kite sends a Unix time in SECONDS. Widen before multiplying: the seconds
/// value is ~1.8e9 and 1e9 nanoseconds per second overflows int32 and even
/// uint32 by nine orders of magnitude.
[[nodiscard]] ALTAIR_HOT constexpr Timestamp
kite_seconds_to_ts(std::uint32_t secs) noexcept {
    return Timestamp{static_cast<std::int64_t>(secs) * 1'000'000'000LL};
}

} // namespace detail

/// Decode one WebSocket binary message. ALTAIR_HOT. Allocates nothing.
///
/// `seq` is the caller's session counter and is advanced once per emitted
/// struct — rule 10's tick_seqno. A buffer shorter than 2 bytes is a
/// HEARTBEAT: zero packets and no error, because Kite sends 1-byte keepalives
/// and treating one as a malformed frame would alarm on normal operation.
[[nodiscard]] ALTAIR_HOT inline std::expected<KiteDecodeResult, KiteDecodeError>
decode_kite_frame(const std::uint8_t* buf, std::size_t len,
                  const SpecStore& store, Timestamp recv_ts,
                  std::uint32_t& seq,
                  Tick* ticks, std::size_t tick_cap,
                  DepthUpdate* depths, std::size_t depth_cap) noexcept {
    KiteDecodeResult r{};
    if (buf == nullptr || len < 2) {
        return r;                                   // heartbeat
    }

    const std::size_t count = detail::be16(buf);

    // Capacity is checked ONCE, up front, before anything is written.
    //
    // A frame of N packets yields at most N ticks and N depth updates, and N
    // is the first field, so this is knowable before the first byte is
    // decoded. Checking mid-stream instead would abort after partially filling
    // the caller's arrays and then return an error carrying no count — every
    // tick already decoded would be lost with no way to know how many there
    // were. All-or-nothing is the only honest option here.
    if (count > tick_cap || count > depth_cap) {
        return std::unexpected(KiteDecodeError::OutputFull);
    }

    std::size_t pos = 2;

    for (std::size_t i = 0; i < count; ++i) {
        // The length header itself must be in bounds before it is read.
        if (pos + 2 > len) {
            return std::unexpected(KiteDecodeError::ShortFrame);
        }
        const std::size_t plen = detail::be16(buf + pos);
        pos += 2;
        if (pos + plen > len) {
            // D7: the framing is lost — everything after this is garbage.
            return std::unexpected(KiteDecodeError::ShortFrame);
        }
        const std::uint8_t* p = buf + pos;
        pos += plen;

        // Every recognised mode is at least 8 bytes, so the token read below
        // is covered by this one check.
        if (plen != detail::kKiteLtpLen && plen != detail::kKiteIndexQuoteLen
            && plen != detail::kKiteIndexFullLen && plen != detail::kKiteQuoteLen
            && plen != detail::kKiteFullLen) {
            // D7: unrecognised mode, but the framing is intact — skip it and
            // keep going. The remaining packets are still readable.
            ++r.bad_length;
            continue;
        }

        const std::uint32_t token = detail::be32(p);
        const KiteSegment seg = kite_segment_of(token);
        const std::int64_t scale = kite_price_scale(seg);

        // D4: a currency derivative is 10^5 times finer than a paisa. Never
        // decoded — truncating would produce a plausible price wrong by five
        // decimal places, and every cost and signal built on it would be wrong
        // the same way. Skipped rather than fatal: see the field comment.
        if (scale != 100) {
            ++r.unsupported_scale;
            continue;
        }

        // D1: the ONLY legitimate way a wire token becomes an instrument.
        const auto id = store.id_of(FeedSource::Kite, token);
        if (!id) {
            // D2: a subscription confirmation can beat the spec store. One
            // unmappable packet must not discard the rest of the frame.
            ++r.unknown_token;
            continue;
        }
        if (store.is_blocked(*id)) {
            // D3: P1-06 already decided not to trade this.
            ++r.blocked;
            continue;
        }

        // Guaranteed by the up-front capacity check: at most `count` ticks.
        const bool is_index = (seg == KiteSegment::Indices);
        Tick& t = ticks[r.ticks];
        t = Tick{};
        t.id = *id;
        t.seq = seq++;
        t.recv_ts = recv_ts;
        t.source = FeedSource::Kite;
        // The wire value IS paise once scale is 100 — no division, no double.
        t.last = Price{static_cast<std::int64_t>(detail::be32(p + 4))};

        if (plen == detail::kKiteLtpLen) {
            // D5: no timestamp in this mode at all.
            t.exchange_ts = recv_ts;
            t.flags = set_flag(t.flags, TickFlag::NoExchangeTs);
        } else if (is_index) {
            // Index layout differs: 8 high, 12 low, 16 open, 20 close.
            // No volume, no OI, no depth — an index is not traded.
            if (plen == detail::kKiteIndexFullLen) {
                t.exchange_ts = detail::kite_seconds_to_ts(detail::be32(p + 28));
            } else {
                t.exchange_ts = recv_ts;
                t.flags = set_flag(t.flags, TickFlag::NoExchangeTs);
            }
        } else {
            t.last_qty = Qty{static_cast<std::int64_t>(detail::be32(p + 8))};
            t.volume   = Qty{static_cast<std::int64_t>(detail::be32(p + 16))};
            if (plen == detail::kKiteFullLen) {
                t.oi = static_cast<std::int64_t>(detail::be32(p + 48));
                t.exchange_ts = detail::kite_seconds_to_ts(detail::be32(p + 60));
            } else {
                // quote mode: no timestamp, no OI.
                t.exchange_ts = recv_ts;
                t.flags = set_flag(t.flags, TickFlag::NoExchangeTs);
            }
        }
        ++r.ticks;

        // Depth rides only on full mode, and never on an index.
        if (plen != detail::kKiteFullLen || is_index) {
            continue;
        }
        // Likewise guaranteed: at most `count` depth updates.
        DepthUpdate& d = depths[r.depths];
        d = DepthUpdate{};
        d.id = t.id;
        d.seq = seq++;
        d.exchange_ts = t.exchange_ts;
        d.recv_ts = recv_ts;
        d.source = FeedSource::Kite;

        // D6: Kite always sends five slots and zero-pads the unused ones. A
        // zeroed level is not a level — a bid at price 0 counted as real is
        // infinite edge. Counting stops at the first zero price.
        for (std::size_t k = 0; k < kDepthLevels; ++k) {
            const std::uint8_t* e = p + detail::kKiteBuyDepthOff
                                  + k * detail::kKiteDepthEntry;
            const std::uint32_t px = detail::be32(e + 4);
            if (px == 0) {
                break;
            }
            d.bid[k].qty    = Qty{static_cast<std::int64_t>(detail::be32(e))};
            d.bid[k].px     = Price{static_cast<std::int64_t>(px)};
            d.bid[k].orders = detail::be16(e + 8);
            ++d.bid_levels;
        }
        for (std::size_t k = 0; k < kDepthLevels; ++k) {
            const std::uint8_t* e = p + detail::kKiteSellDepthOff
                                  + k * detail::kKiteDepthEntry;
            const std::uint32_t px = detail::be32(e + 4);
            if (px == 0) {
                break;
            }
            d.ask[k].qty    = Qty{static_cast<std::int64_t>(detail::be32(e))};
            d.ask[k].px     = Price{static_cast<std::int64_t>(px)};
            d.ask[k].orders = detail::be16(e + 8);
            ++d.ask_levels;
        }
        ++r.depths;
    }

    return r;
}

} // namespace altair
