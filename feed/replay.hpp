#pragma once

// P0-09b — a replayer that physically cannot expose a future tick.
//
// CLAUDE.md rule 7: no look-ahead, ever — "a replayer that physically cannot
// expose a future tick", and "strategies read time off the tick, never from a
// wall clock". Rule 6: backtest and live share the same code path.
//
// Look-ahead is the most expensive bug in this domain because it does not
// crash. It produces a BETTER result, and the better result is what gets
// believed. A replayer that exposes ticks[i+1] will eventually have someone
// read it, and no test of the strategy will catch it.
//
// So the guarantee lives in the type. There is no peek(), no at(), no
// operator[], no begin(), no data(), no iterator. The only way to obtain a
// tick is next(), which advances the cursor. A strategy cannot read ahead
// because the interface offers no means to — not because a comment asks it not
// to.

#include <invariant/conservation.hpp>
#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a replay step failed.
// ─────────────────────────────────────────────────────────────────────────
enum class ReplayError : std::uint8_t {
    Exhausted,      // no ticks remain
    Empty,          // the replayer holds no ticks at all
    NotMonotonic    // the source is out of order — it is not a replayable session
};

// ─────────────────────────────────────────────────────────────────────────
// ReplayTick — the SKELETON tick. P2-01 defines the real normalised Tick and
// DepthUpdate; this carries the minimum a null strategy needs so the Phase 0
// exit criterion can run end to end.
//
// Trivially copyable and 40 bytes, so it can ride an SpscRing unchanged when
// the live path arrives.
// ─────────────────────────────────────────────────────────────────────────
struct ReplayTick {
    /// Exchange instant. UNIT: nanoseconds since the Unix epoch, UTC.
    Timestamp ts;
    /// Monotonic sequence number within the session. UNIT: none.
    std::uint64_t seqno;
    /// Instrument token. UNIT: none. A token, never a symbol string.
    ///
    /// WARNING — P2-01 MUST NOT INHERIT THIS FIELD AS-IS. A broker token is
    /// not an instrument key. Kite's `instrument_token` and XTS's
    /// `ExchangeInstrumentID` are different number spaces for the same
    /// contract, so this field means different things depending on which feed
    /// produced the tick. P2-05 switches the primary feed mid-session; with a
    /// broker token in the tick, every instrument would appear to vanish and a
    /// stranger appear in its place, and the book and ledger would follow.
    ///
    /// The normalised Tick must carry the canonical `InstrumentId` assigned by
    /// the spec store (ROADMAP §6.2). Decoders map (source, broker_token) ->
    /// InstrumentId; nothing downstream ever sees a broker token. That is what
    /// P2-04 exists for. This field is a Phase 0 skeleton and is single-source
    /// by construction — one synthetic generator, one token space.
    std::uint32_t token;
    std::uint32_t reserved;
    /// Last traded price. UNIT: paise.
    Price last;
    /// Last traded quantity. UNIT: units.
    Qty qty;
};

static_assert(sizeof(ReplayTick) == 40, "ReplayTick must stay 40 bytes");
static_assert(alignof(ReplayTick) >= 8, "ReplayTick must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<ReplayTick>,
              "ReplayTick must ride an SpscRing unchanged");

// ─────────────────────────────────────────────────────────────────────────
// Replayer — a cursor over a session, and nothing more.
//
// THERE IS DELIBERATELY NO peek(), at(), operator[], begin(), OR ANY OTHER
// WAY TO SEE A TICK THE CURSOR HAS NOT REACHED.
//
// Borrows the tick array; the caller owns it and must outlive the Replayer.
// Not thread-safe: one replayer per replay thread.
// ─────────────────────────────────────────────────────────────────────────
class Replayer {
public:
    /// An empty replayer. Every next() returns Empty.
    constexpr Replayer() noexcept = default;

    /// Borrow a session. UNIT: none.
    /// PRECONDITION: `ticks` points to `n` ticks and outlives this object.
    /// Does NOT validate ordering — call validate() for that, once, up front.
    constexpr Replayer(const ReplayTick* ticks, std::size_t n) noexcept
        : ticks_(ticks), n_(ticks == nullptr ? 0 : n) {}

    /// Deliver the next tick and advance. UNIT: none.
    /// Returns Empty when the replayer holds nothing, Exhausted at the end,
    /// and NotMonotonic if the next tick would move seqno or time backwards —
    /// a corrupt source must stop the replay, not silently reorder it.
    /// PRECONDITION: called from exactly one thread.
    [[nodiscard]] ALTAIR_HOT std::expected<ReplayTick, ReplayError> next() noexcept {
        if (ticks_ == nullptr || n_ == 0) {
            // Distinct from Exhausted: an empty source is a setup bug, a
            // finished one is normal termination.
            return std::unexpected(ReplayError::Empty);
        }
        if (cursor_ >= n_) {
            return std::unexpected(ReplayError::Exhausted);
        }

        const ReplayTick t = ticks_[cursor_];

        if (started_) {
            // seqno must STRICTLY increase; timestamps may REPEAT, because two
            // trades can share a nanosecond but two ticks cannot share a
            // sequence number.
            if (t.seqno <= last_seqno_ || t.ts < now_) {
                return std::unexpected(ReplayError::NotMonotonic);   // no advance
            }
        }

        ++cursor_;
        now_ = t.ts;
        last_seqno_ = t.seqno;
        started_ = true;
        // BY VALUE. A reference into the array would let a caller do pointer
        // arithmetic and walk forward.
        return t;
    }

    /// The current market instant: the timestamp of the tick LAST DELIVERED.
    /// UNIT: nanoseconds since the Unix epoch, UTC.
    /// Timestamp::epoch() before the first delivery. NEVER the next tick's
    /// time, and never a wall clock — this is the only clock a strategy may
    /// read during replay (CLAUDE.md rule 7).
    [[nodiscard]] ALTAIR_HOT Timestamp now() const noexcept { return now_; }

    /// Ticks delivered so far. UNIT: count.
    [[nodiscard]] constexpr std::size_t delivered() const noexcept { return cursor_; }

    /// Ticks not yet delivered. UNIT: count.
    [[nodiscard]] constexpr std::size_t remaining() const noexcept { return n_ - cursor_; }

    /// Total ticks in the session. UNIT: count.
    [[nodiscard]] constexpr std::size_t size() const noexcept { return n_; }

    /// True iff every tick has been delivered. UNIT: none.
    [[nodiscard]] constexpr bool exhausted() const noexcept { return cursor_ >= n_; }

    /// Verify the WHOLE source is monotonic in both seqno and timestamp.
    /// UNIT: none. O(n) — call once before replaying, never on the hot path.
    /// Returns Empty for an empty source, NotMonotonic on the first violation.
    [[nodiscard]] std::expected<void, ReplayError> validate() const noexcept {
        if (ticks_ == nullptr || n_ == 0) {
            return std::unexpected(ReplayError::Empty);
        }
        for (std::size_t i = 1; i < n_; ++i) {
            if (ticks_[i].seqno <= ticks_[i - 1].seqno) {
                return std::unexpected(ReplayError::NotMonotonic);
            }
            if (ticks_[i].ts < ticks_[i - 1].ts) {
                return std::unexpected(ReplayError::NotMonotonic);
            }
        }
        return {};
    }

    /// Return the cursor to the start. UNIT: none. For a second pass over the
    /// same session — walk-forward folds in P6-06 will need it.
    void rewind() noexcept {
        cursor_ = 0;
        now_ = Timestamp::epoch();
        last_seqno_ = 0;
        started_ = false;
    }

private:
    const ReplayTick* ticks_ = nullptr;
    std::size_t n_ = 0;
    std::size_t cursor_ = 0;
    Timestamp now_{};
    std::uint64_t last_seqno_ = 0;
    bool started_ = false;
};

} // namespace altair
