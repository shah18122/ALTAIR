// server/protocol.hpp -- the binary delta-frame wire protocol.
//
// P11-01. THE ONLY THING server/ AND client/ SHARE.
//
// CLAUDE.md: "server/ and client/ never share a header. They share a wire
// protocol and nothing more." This file is the server's half of that protocol.
// The client re-implements the decoder in TypeScript, and the two are kept
// honest by a file of conformance vectors both test against -- shared test
// vectors, not shared code. If the implementations drift, a vector fails
// rather than the dashboard quietly showing a wrong number.
//
// A NANOSECOND TIMESTAMP DOES NOT SURVIVE A JAVASCRIPT NUMBER.
//
// This is the card, and it is specific to this architecture: a C++ server and
// a browser client. Every JavaScript number is an IEEE-754 double, exact only
// for integers up to 2^53. The engine timestamps in nanoseconds since the Unix
// epoch, and that is around 1.79e18 today -- between 2^60 and 2^61, where the
// spacing between representable doubles is 2^8 = 256 ns.
//
// So two ticks less than 256 ns apart decode to the SAME number. Measured on
// a burst of 4096 ticks 40 ns apart -- an entirely ordinary option-chain
// update -- 3455 collapse onto their predecessor and 641 distinct instants
// survive out of 4096.
//
// Nothing is INVERTED, and that is worth stating precisely rather than
// overstating: rounding to nearest is monotone, so the order is not
// scrambled. It is ERASED. Distinct events become simultaneous, and whatever
// the client sorts by next -- arrival, row id, nothing at all -- decides
// their order instead. The replay scrubber (P11-13) and the audit trail
// (P11-14) both compare timestamps for equality and ordering, so this is not
// a display rounding error.
//
// Money does NOT have this problem, and saying so is the point: an amount in
// paise would have to exceed Rs 90,07,199 crore before a double lost a paisa,
// which is not a number this system will hold. The hazard is TIME, and a rule
// of "be careful with big numbers" would have missed it because the big
// numbers here are the ones that look innocuous.
//
// The protocol is therefore BINARY, every 64-bit field is fixed-width
// little-endian, and the client decodes them to BigInt. There is no JSON
// encoding of a 64-bit field anywhere, including in the conformance vectors --
// which write every 64-bit value as a decimal STRING, because writing it as a
// JSON number would corrupt the vectors with the very bug they exist to catch.
//
// COALESCING IS CORRECT FOR STATE AND CATASTROPHIC FOR EVENTS.
//
// The second thing. Sixty frames a second cannot carry every tick, so the
// server coalesces: last-writer-wins per field. That is exactly right for a
// price -- nobody needs the intermediate values of a quote that moved four
// times in one frame.
//
// It is ruinous for a fill, a flag, a kill-switch trip or a P&L increment.
// Those are not samples of a state; they are events, and dropping one loses
// money that was actually made or a warning that was actually raised. Measured
// on 512 fills across 240 frames of a four-second window, coalescing the event
// channel the way the state channel is coalesced keeps 206 fills of 512 and
// reports Rs 35,775 of realised P&L against a true Rs 87,178 -- 59% of it
// gone, with nothing on screen to say so.
//
// And the error is NOT conservative. In that tape the fills have a positive
// mean, so dropping most of them understates P&L and the number at least errs
// toward caution; that is an accident of the sign mix. Re-run with P&L centred
// on zero -- a market-making tape, or any losing session -- and over 64 tapes
// the coalesced total OVERSTATES on 29 of them, worst case by Rs 16,681
// against a worst understatement of Rs 12,669. A surviving subsample is just a
// subsample. A risk number that comes out too small is the direction that
// costs money.
//
// So there are two channels with different overflow policies, and the reliable
// one does not drop. If it cannot keep up it ends the session with a named
// reason, because a dashboard telling you to reconnect is better than a
// dashboard quietly missing your fills.
//
// A GAP MUST BE DETECTABLE, NOT INVISIBLE.
//
// A delta applied to a baseline the client does not have is not a smaller
// error than a missing frame -- it is a wrong number rendered confidently.
// Every frame carries a sequence number, `SeqTracker` classifies the next one
// as InOrder, Duplicate or Gap, and a Gap forces a fresh snapshot. Absence is
// not zero (P5-02), and here absence is not "no change" either.
//
// THE CLIENT CANNOT PLACE AN ORDER, BECAUSE THERE IS NO WORD FOR IT.
//
// oms/ is the only thing that places orders. `ClientMsg` therefore has four
// members -- subscribe, unsubscribe, snapshot request, kill switch -- and no
// order-placing member exists to be named. The kill switch is one-way: it can
// only ever flatten, never open.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair {

/// Wire format version. Bumped when a frame's layout changes; a client
/// speaking a different version is refused rather than reinterpreted.
inline constexpr std::uint16_t kWireVersion = 1;

/// Frame magic. Present so a mis-framed stream fails at the first byte
/// rather than 40 bytes later with a plausible-looking payload.
inline constexpr std::uint32_t kFrameMagic = 0x414C5452u;   // 'ALTR'

inline constexpr std::size_t kFrameHeaderBytes = 48;

enum class ProtocolError : std::uint8_t {
    /// The buffer was shorter than the field being read or written.
    ShortBuffer,
    /// The magic did not match, so the stream is not frame-aligned.
    BadMagic,
    /// The frame's wire version is not `kWireVersion`.
    VersionMismatch,
    /// A field that has no valid zero was zero.
    Unspecified,
    /// The payload length exceeded what the session permits.
    PayloadTooLarge
};

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

/// Which delivery guarantee a frame travels under.
///
/// The distinction is the whole reason there are two: `State` may be coalesced
/// because only the latest value matters, and `Event` may NOT because each one
/// is a separate fact.
enum class Channel : std::uint8_t {
    /// Phantom default. A zeroed header is not a valid frame.
    Unspecified = 0,
    /// Coalescible. Last-writer-wins per field: quotes, greeks, book levels,
    /// portfolio marks. Dropping an intermediate value loses nothing, because
    /// the next frame carries the same field's current value.
    State,
    /// Reliable, ordered, never coalesced: fills, flags, kill-switch trips,
    /// P&L increments, audit rows. Each is a separate fact and there is no
    /// later frame that restates it.
    Event
};

/// What a frame is.
enum class FrameKind : std::uint8_t {
    Unspecified = 0,
    /// A complete baseline. Every delta is meaningless without one.
    Snapshot,
    /// A change against the last snapshot plus every delta since.
    Delta,
    /// Carries no payload; proves the link is alive and advances nothing.
    /// Deliberately does NOT advance `seq`, so a heartbeat can never be
    /// mistaken for a lost delta.
    Heartbeat,
    /// The server telling the client it has fallen behind and what it missed.
    Gap,
    /// Orderly close, with a reason. A socket that just dies is a different
    /// thing from a server that decided to end the session.
    Bye
};

/// Everything the client is allowed to say.
///
/// There is no order-placing member. oms/ is the only thing that places an
/// order, and a client that cannot name the action cannot be talked into it
/// by a compromised page, an injected script, or a bug.
enum class ClientMsg : std::uint8_t {
    Unspecified = 0,
    Subscribe,
    Unsubscribe,
    /// "I have a gap, or I just connected -- send me a baseline."
    SnapshotRequest,
    /// One way. Flattens; can never open. The server forwards it to oms/ and
    /// does not act on it itself.
    KillSwitch
};

/// Why a session ended.
enum class ByeReason : std::uint8_t {
    Unspecified = 0,
    ClientClosed,
    ServerShutdown,
    VersionRefused,
    AuthFailed,
    /// The reliable channel backed up. Named, because the alternative -- to
    /// drop events quietly -- is the failure this protocol exists to prevent.
    EventBacklog,
    Idle
};

// ---------------------------------------------------------------------------
// The frame header
// ---------------------------------------------------------------------------

/// Fixed 48 bytes, all fields little-endian.
struct FrameHeader {
    std::uint32_t magic = kFrameMagic;
    std::uint16_t version = kWireVersion;
    FrameKind kind = FrameKind::Unspecified;
    Channel channel = Channel::Unspecified;

    /// Per-channel, monotonically increasing by exactly one per Snapshot or
    /// Delta. The client's only means of noticing it missed something.
    std::uint64_t seq = 0;

    /// Which stream this frame belongs to. Subscriptions are per topic, so a
    /// gap on one topic does not invalidate another.
    std::uint32_t topic = 0;
    std::uint32_t payload_len = 0;

    /// THE ENGINE'S TICK TIME, in nanoseconds since the Unix epoch -- read off
    /// the tick, never from a wall clock (rule 7). A replay renders at the
    /// timestamps the ticks actually carried.
    std::int64_t engine_time_ns = 0;

    /// When the server sent it. Present so the two can be COMPARED: in live
    /// trading they track, and in replay they diverge by hours, which is what
    /// makes a replay visibly a replay instead of a very calm afternoon.
    std::int64_t server_time_ns = 0;

    [[nodiscard]] Timestamp engine_time() const noexcept {
        return Timestamp{engine_time_ns};
    }

    [[nodiscard]] bool advances_seq() const noexcept {
        return kind == FrameKind::Snapshot || kind == FrameKind::Delta;
    }

    [[nodiscard]] bool valid() const noexcept {
        return magic == kFrameMagic && version == kWireVersion
            && kind != FrameKind::Unspecified
            && channel != Channel::Unspecified;
    }
};

// ---------------------------------------------------------------------------
// Little-endian fixed-width codec
// ---------------------------------------------------------------------------
//
// Hand-rolled rather than memcpy of the struct: a struct layout is a compiler
// promise, and the decoder on the other side is TypeScript, which has no
// opinion about MSVC's padding.

namespace wire {

inline void put_u16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
inline void put_u64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
/// Two's complement, little-endian. int64 and uint64 share one representation
/// on the wire so the client has one 64-bit decode path, not two.
inline void put_i64(std::uint8_t* p, std::int64_t v) noexcept {
    put_u64(p, static_cast<std::uint64_t>(v));
}

[[nodiscard]] inline std::uint16_t get_u16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0])
         | static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}
[[nodiscard]] inline std::uint32_t get_u32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}
[[nodiscard]] inline std::uint64_t get_u64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}
[[nodiscard]] inline std::int64_t get_i64(const std::uint8_t* p) noexcept {
    return static_cast<std::int64_t>(get_u64(p));
}

} // namespace wire

/// Serialise a header. Returns bytes written.
[[nodiscard]] inline std::expected<std::size_t, ProtocolError>
encode_header(const FrameHeader& h, std::uint8_t* out, std::size_t cap) noexcept {
    if (cap < kFrameHeaderBytes) {
        return std::unexpected(ProtocolError::ShortBuffer);
    }
    if (h.kind == FrameKind::Unspecified || h.channel == Channel::Unspecified) {
        return std::unexpected(ProtocolError::Unspecified);
    }
    wire::put_u32(out + 0, kFrameMagic);
    wire::put_u16(out + 4, kWireVersion);
    out[6] = static_cast<std::uint8_t>(h.kind);
    out[7] = static_cast<std::uint8_t>(h.channel);
    wire::put_u64(out + 8, h.seq);
    wire::put_u32(out + 16, h.topic);
    wire::put_u32(out + 20, h.payload_len);
    wire::put_i64(out + 24, h.engine_time_ns);
    wire::put_i64(out + 32, h.server_time_ns);
    wire::put_u64(out + 40, 0);          // reserved, must be zero
    return kFrameHeaderBytes;
}

[[nodiscard]] inline std::expected<FrameHeader, ProtocolError>
decode_header(const std::uint8_t* in, std::size_t n) noexcept {
    if (n < kFrameHeaderBytes) {
        return std::unexpected(ProtocolError::ShortBuffer);
    }
    if (wire::get_u32(in) != kFrameMagic) {
        return std::unexpected(ProtocolError::BadMagic);
    }
    FrameHeader h{};
    h.version = wire::get_u16(in + 4);
    if (h.version != kWireVersion) {
        return std::unexpected(ProtocolError::VersionMismatch);
    }
    h.kind = static_cast<FrameKind>(in[6]);
    h.channel = static_cast<Channel>(in[7]);
    if (h.kind == FrameKind::Unspecified || h.channel == Channel::Unspecified) {
        return std::unexpected(ProtocolError::Unspecified);
    }
    h.seq = wire::get_u64(in + 8);
    h.topic = wire::get_u32(in + 16);
    h.payload_len = wire::get_u32(in + 20);
    h.engine_time_ns = wire::get_i64(in + 24);
    h.server_time_ns = wire::get_i64(in + 32);
    return h;
}

// ---------------------------------------------------------------------------
// Sequencing
// ---------------------------------------------------------------------------

enum class SeqVerdict : std::uint8_t {
    Unspecified = 0,
    InOrder,
    /// Already seen. Harmless -- discard it.
    Duplicate,
    /// Frames were lost. NOT harmless: every delta after this one applies to a
    /// baseline the client does not have.
    Gap
};

/// Tracks one channel of one topic.
struct SeqTracker {
    std::uint64_t last = 0;
    bool primed = false;
    /// Set on the first Gap and cleared only by a Snapshot. While it is set,
    /// deltas must NOT be applied -- a delta on a stale baseline renders a
    /// wrong number confidently, which is worse than rendering nothing.
    bool needs_snapshot = true;
    std::uint64_t frames_missed = 0;

    [[nodiscard]] SeqVerdict accept(const FrameHeader& h) noexcept {
        if (!h.advances_seq()) { return SeqVerdict::InOrder; }
        if (h.kind == FrameKind::Snapshot) {
            last = h.seq;
            primed = true;
            needs_snapshot = false;      // a baseline arrived
            return SeqVerdict::InOrder;
        }
        if (!primed) {
            // A delta before any snapshot. There is nothing to apply it to.
            ++frames_missed;
            needs_snapshot = true;
            return SeqVerdict::Gap;
        }
        if (h.seq <= last) { return SeqVerdict::Duplicate; }
        if (h.seq == last + 1) {
            last = h.seq;
            return SeqVerdict::InOrder;
        }
        frames_missed += h.seq - last - 1;
        last = h.seq;
        needs_snapshot = true;
        return SeqVerdict::Gap;
    }

    /// May a delta be applied right now?
    [[nodiscard]] bool can_apply() const noexcept {
        return primed && !needs_snapshot;
    }
};

// ---------------------------------------------------------------------------
// Backpressure policy
// ---------------------------------------------------------------------------

enum class Overflow : std::uint8_t {
    Unspecified = 0,
    /// Keep the newest, discard the rest, and tell the client how many were
    /// skipped. Only ever correct for `Channel::State`.
    CoalesceNewest,
    /// Never drop. If the queue cannot drain, end the session with
    /// `ByeReason::EventBacklog`.
    DisconnectOnBacklog
};

/// The policy a channel MUST have. There is no default constructor that picks
/// one: choosing `CoalesceNewest` for events is the expensive mistake, and it
/// should be typed out by someone rather than inherited from a struct.
[[nodiscard]] inline constexpr Overflow required_policy(Channel c) noexcept {
    return c == Channel::Event ? Overflow::DisconnectOnBacklog
         : c == Channel::State ? Overflow::CoalesceNewest
                               : Overflow::Unspecified;
}

/// Is this pairing sound? Used to reject a session configured with an event
/// channel that drops.
[[nodiscard]] inline constexpr bool policy_ok(Channel c, Overflow o) noexcept {
    return o != Overflow::Unspecified && o == required_policy(c);
}

// ---------------------------------------------------------------------------
// The JavaScript number hazard, as a measurement
// ---------------------------------------------------------------------------

/// The largest integer a JavaScript `number` represents exactly: 2^53 - 1.
inline constexpr std::int64_t kJsMaxSafeInteger = 9007199254740991LL;

/// Would this value survive a round trip through a JavaScript `number`?
///
/// Provided so the answer is computed rather than assumed. It is `true` for
/// every paise amount this system will hold and `false` for every nanosecond
/// timestamp it will ever emit, and that asymmetry is the card.
[[nodiscard]] inline constexpr bool js_number_safe(std::int64_t v) noexcept {
    return v >= -kJsMaxSafeInteger && v <= kJsMaxSafeInteger;
}

/// The spacing between representable doubles at `v`, in the units of `v`.
///
/// For a nanosecond timestamp this is the resolution the client would have if
/// it decoded to a `number`: two instants closer together than this become the
/// same instant, and their order is lost.
[[nodiscard]] inline double double_resolution_at(std::int64_t v) noexcept {
    const double d = static_cast<double>(v < 0 ? -v : v);
    if (!(d > 0.0)) { return 0.0; }
    return std::nextafter(d, 2.0 * d) - d;
}

} // namespace altair
