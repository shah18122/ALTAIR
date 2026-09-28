// book/queue_position.hpp -- M20 deterministic order-level queue replay.
#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class QueueEventKind : std::uint8_t {
    Snapshot,
    AddAhead,
    CancelAhead,
    ExecuteAtLevel,
    CancelOwn
};

struct QueueEvent {
    std::uint64_t sequence = 0;
    QueueEventKind kind = QueueEventKind::Snapshot;
    std::uint64_t quantity = 0;
};

enum class QueuePositionError : std::uint8_t {
    BadQuantity,
    SequenceGap,
    SequenceDuplicate,
    Overflow,
    Terminal
};

struct QueueState {
    std::uint64_t ahead = 0;
    std::uint64_t own_remaining = 0;
    std::uint64_t own_filled = 0;
    std::uint64_t last_sequence = 0;
    bool initialised = false;
    bool cancelled = false;
    bool valid = true;
};

/// Exact FIFO queue position. It accepts only events whose feed semantics say
/// the add/cancel occurred ahead; level-only data cannot supply that fact and
/// must not call this model. A gap permanently invalidates the state until a
/// new snapshot object is constructed.
class QueuePosition {
public:
    explicit QueuePosition(std::uint64_t own_quantity) noexcept {
        state_.own_remaining = own_quantity;
        state_.valid = own_quantity != 0;
    }

    [[nodiscard]] std::expected<std::uint64_t, QueuePositionError>
    apply(const QueueEvent& event) noexcept {
        if (!state_.valid || state_.cancelled || state_.own_remaining == 0)
            return std::unexpected(QueuePositionError::Terminal);
        if (event.quantity == 0 && event.kind != QueueEventKind::CancelOwn)
            return std::unexpected(QueuePositionError::BadQuantity);
        if (state_.initialised) {
            if (event.sequence == state_.last_sequence)
                return std::unexpected(QueuePositionError::SequenceDuplicate);
            if (event.sequence != state_.last_sequence + 1) {
                state_.valid = false;
                return std::unexpected(QueuePositionError::SequenceGap);
            }
        }
        state_.last_sequence = event.sequence;
        state_.initialised = true;
        std::uint64_t fill = 0;
        switch (event.kind) {
            case QueueEventKind::Snapshot:
                state_.ahead = event.quantity;
                break;
            case QueueEventKind::AddAhead:
                if (state_.ahead > std::numeric_limits<std::uint64_t>::max()
                                   - event.quantity)
                    return std::unexpected(QueuePositionError::Overflow);
                state_.ahead += event.quantity;
                break;
            case QueueEventKind::CancelAhead:
                state_.ahead -= std::min(state_.ahead, event.quantity);
                break;
            case QueueEventKind::ExecuteAtLevel: {
                const std::uint64_t consume_ahead = std::min(state_.ahead,
                                                             event.quantity);
                state_.ahead -= consume_ahead;
                const std::uint64_t remainder = event.quantity - consume_ahead;
                fill = std::min(state_.own_remaining, remainder);
                state_.own_remaining -= fill;
                state_.own_filled += fill;
                break;
            }
            case QueueEventKind::CancelOwn:
                state_.cancelled = true;
                break;
        }
        return fill;
    }

    [[nodiscard]] const QueueState& state() const noexcept { return state_; }

private:
    QueueState state_{};
};

} // namespace altair
