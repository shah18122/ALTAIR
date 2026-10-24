// server/price_payload.hpp -- what rides inside a price frame.
//
// P37-01. protocol.hpp defines the 48-byte envelope: magic, version, kind,
// channel, sequence, topic, length and two clocks. It deliberately says
// nothing about payloads -- `protocol.hpp:138` names "quotes, greeks, book
// levels, portfolio marks" as things the State channel would carry and leaves
// every one of them unwritten.
//
// This is the first of them, and it is the one the whole live path needs: a
// tick, and optionally the five levels of book behind it.
//
// WHY A SECOND STRUCT INSTEAD OF MEMCPY-ING feed::Tick.
//
// feed/tick.hpp pins Tick at 64 bytes with a static_assert and DepthUpdate at
// 272, which is exactly what you want in the engine and exactly what you must
// not put on a wire. Those layouts carry padding, an enum whose underlying
// type is a compiler decision, and an InstrumentId whose meaning is local to
// one process's spec store. Writing them raw would make the protocol a
// function of the compiler that built the server.
//
// So the wire format is written out field by field, little-endian, fixed
// width -- and the CONFORMANCE VECTORS in tests/vectors are the contract,
// exactly as `server/` and a remote client already share vectors and never a
// header. If the two sides drift, the vectors fail rather than a dashboard
// quietly showing a wrong number.
//
// THE TOKEN IS ON THE WIRE, NOT THE InstrumentId.
//
// InstrumentId is assigned by whichever process loaded the spec store and is
// meaningless to anybody else. The broker's numeric token is the thing both
// ends can independently resolve, so that is what crosses. This is the same
// reason feed/tick.hpp keeps the two separate in the first place.
//
// ABSENCE IS NOT ZERO, AND THAT IS THE WHOLE REASON FOR THE FLAGS WORD.
//
// An index has no volume, no open interest and no book. A zero in those fields
// would be indistinguishable from a real zero -- an instrument that genuinely
// traded nothing this second. Every optional field therefore has a presence
// bit, and a reader that ignores the bits gets zeros it was warned about.

#pragma once

#include <server/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Topics. A subscription is per topic, and a gap on one does not invalidate
/// another -- so trades and book are separate streams even though they
/// describe the same instrument.
inline constexpr std::uint32_t kTopicTrades = 1;
inline constexpr std::uint32_t kTopicBook   = 2;

/// Presence bits. See the header note: a zero with no bit beside it is a
/// measured zero, and a zero with the bit clear is an absence.
enum PriceFlags : std::uint16_t {
    kPriceHasVolume = 1u << 0,
    kPriceHasOi     = 1u << 1,
    kPriceHasBook   = 1u << 2,
    /// The exchange gave no timestamp of its own, so engine_time is a receive
    /// time. Carried through rather than papered over: a bar built from
    /// receive times is a different measurement.
    kPriceNoExchTs  = 1u << 3,
    /// Replay, not live. The frame is real data at its original timestamps and
    /// must never be mistaken for now.
    kPriceReplay    = 1u << 4,
    /// Simulated, not market data at all: altair_price_service --sim. Shown
    /// as SIM and never as LIVE, for the same reason as a replay.
    kPriceSimulated = 1u << 5
};

/// One price update on the wire. 48 bytes, fixed.
struct PricePayload {
    std::uint32_t token = 0;
    std::uint16_t flags = 0;
    std::uint16_t depth_levels = 0;   ///< 0..5, and 0 when kPriceHasBook is clear
    std::int64_t  last_paise = 0;
    std::int64_t  last_qty = 0;
    std::int64_t  volume = 0;
    std::int64_t  oi = 0;
    std::int64_t  exchange_ts_ns = 0;

    [[nodiscard]] bool has(PriceFlags f) const noexcept {
        return (flags & static_cast<std::uint16_t>(f)) != 0;
    }
};

/// One side of one level.
struct PriceLevel {
    std::int64_t  price_paise = 0;
    std::int64_t  qty = 0;
    std::uint32_t orders = 0;
    std::uint32_t reserved = 0;
};

inline constexpr std::size_t kPricePayloadBytes = 48;
inline constexpr std::size_t kPriceLevelBytes = 24;
/// Fifty: the FYERS 50-level book (broker/fyers_tbt_socket.hpp). The HSM and
/// Kite feeds send five, and a frame carries only the levels it has.
inline constexpr std::size_t kMaxDepthLevels = 50;

/// A book frame is the payload followed by bid levels then ask levels, each
/// `depth_levels` long. Both sides carry the SAME count: a book with three
/// bids and one ask is padded to three with zero-quantity levels rather than
/// sent ragged, because a reader that has to guess where the asks start is a
/// reader that will one day guess wrong.
[[nodiscard]] inline constexpr std::size_t
price_frame_bytes(std::uint16_t depth_levels) noexcept {
    return kPricePayloadBytes
           + 2u * static_cast<std::size_t>(depth_levels) * kPriceLevelBytes;
}

// ---------------------------------------------------------------------------
// Codec. Little-endian fixed width, reusing protocol.hpp's wire helpers so
// there is exactly one place that decides byte order.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::expected<std::size_t, ProtocolError>
encode_price(const PricePayload& p, const PriceLevel* bids,
             const PriceLevel* asks, std::uint8_t* out,
             std::size_t cap) noexcept {
    const std::uint16_t levels =
        p.has(kPriceHasBook) ? p.depth_levels : static_cast<std::uint16_t>(0);
    if (levels > kMaxDepthLevels) {
        // RULE 11: refuse. A book deeper than the wire format allows is a
        // caller error, and truncating it here would publish a five-level
        // book as if it were the whole thing.
        return std::unexpected(ProtocolError::Unspecified);
    }
    if (levels > 0 && (bids == nullptr || asks == nullptr)) {
        return std::unexpected(ProtocolError::Unspecified);
    }
    const std::size_t need = price_frame_bytes(levels);
    if (cap < need) { return std::unexpected(ProtocolError::ShortBuffer); }

    wire::put_u32(out + 0, p.token);
    wire::put_u16(out + 4, p.flags);
    wire::put_u16(out + 6, levels);
    wire::put_i64(out + 8, p.last_paise);
    wire::put_i64(out + 16, p.last_qty);
    wire::put_i64(out + 24, p.volume);
    wire::put_i64(out + 32, p.oi);
    wire::put_i64(out + 40, p.exchange_ts_ns);

    std::size_t at = kPricePayloadBytes;
    for (int side = 0; side < 2; ++side) {
        const PriceLevel* src = side == 0 ? bids : asks;
        for (std::uint16_t i = 0; i < levels; ++i) {
            wire::put_i64(out + at + 0, src[i].price_paise);
            wire::put_i64(out + at + 8, src[i].qty);
            wire::put_u32(out + at + 16, src[i].orders);
            wire::put_u32(out + at + 20, 0u);
            at += kPriceLevelBytes;
        }
    }
    return need;
}

struct DecodedPrice {
    PricePayload payload;
    PriceLevel bids[kMaxDepthLevels]{};
    PriceLevel asks[kMaxDepthLevels]{};
    std::size_t bytes = 0;
};

[[nodiscard]] inline std::expected<DecodedPrice, ProtocolError>
decode_price(const std::uint8_t* in, std::size_t n) noexcept {
    if (in == nullptr || n < kPricePayloadBytes) {
        return std::unexpected(ProtocolError::ShortBuffer);
    }
    DecodedPrice d;
    d.payload.token = wire::get_u32(in + 0);
    d.payload.flags = wire::get_u16(in + 4);
    d.payload.depth_levels = wire::get_u16(in + 6);
    d.payload.last_paise = wire::get_i64(in + 8);
    d.payload.last_qty = wire::get_i64(in + 16);
    d.payload.volume = wire::get_i64(in + 24);
    d.payload.oi = wire::get_i64(in + 32);
    d.payload.exchange_ts_ns = wire::get_i64(in + 40);

    if (d.payload.depth_levels > kMaxDepthLevels) {
        // A LENGTH READ OFF THE WIRE IS UNTRUSTED. This is the field an
        // attacker or a bug controls, and believing it would read past the
        // buffer. Refuse rather than clamp: a clamped book is a wrong book
        // that looks right.
        return std::unexpected(ProtocolError::Unspecified);
    }
    // A count with the presence bit clear is a contradiction; so is the
    // reverse. Either would leave the reader guessing which field to trust.
    const bool has_book = d.payload.has(kPriceHasBook);
    if (has_book != (d.payload.depth_levels > 0)) {
        return std::unexpected(ProtocolError::Unspecified);
    }

    const std::size_t need = price_frame_bytes(d.payload.depth_levels);
    if (n < need) { return std::unexpected(ProtocolError::ShortBuffer); }

    std::size_t at = kPricePayloadBytes;
    for (int side = 0; side < 2; ++side) {
        PriceLevel* dst = side == 0 ? d.bids : d.asks;
        for (std::uint16_t i = 0; i < d.payload.depth_levels; ++i) {
            dst[i].price_paise = wire::get_i64(in + at + 0);
            dst[i].qty = wire::get_i64(in + at + 8);
            dst[i].orders = wire::get_u32(in + at + 16);
            dst[i].reserved = 0;
            at += kPriceLevelBytes;
        }
    }
    d.bytes = need;
    return d;
}

}  // namespace altair
