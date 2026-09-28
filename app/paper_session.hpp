// app/paper_session.hpp
// P7-08. Deterministic orchestration around the isolated PaperVenue.
#pragma once

#include <broker/account_snapshot.hpp>
#include <core/types/signed_sum.hpp>
#include <oms/paper_venue.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

namespace altair::app {

enum class PaperSessionError : std::uint8_t {
    BadConfig, SubmitRefused, QuoteRefused, RecoveryMismatch, TooManyPositions
};

struct PaperSessionConfig {
    Notional opening_cash{};
    oms::PaperConfig venue{};
    broker_view::SessionKey session{};
    broker_view::InstrumentKey instrument{broker_view::InstrumentKey::None};
    broker_view::PositionProduct product{broker_view::PositionProduct::Unknown};
    LotSize lot_size{};
    std::array<char, broker_view::kBrokerAccountIdMax + 1> account_id{};
};

enum class PaperRecordKind : std::uint8_t { Submit, Quote };

struct PaperRecord {
    PaperRecordKind kind{PaperRecordKind::Quote};
    oms::OrderIntent intent{};
    oms::PaperQuote quote{};
    Timestamp now{};
};

/// A paper-only session. It accepts normalised intents/quotes, journals every
/// accepted input, and can rebuild from that journal. It has no broker client,
/// credential, socket or live dispatch gate.
class PaperSession final {
public:
    explicit PaperSession(PaperSessionConfig config)
        : config_(std::move(config)), venue_(config_.opening_cash, config_.venue) {}

    [[nodiscard]] bool valid() const noexcept {
        return broker_view::valid(config_.session)
            && config_.instrument != broker_view::InstrumentKey::None
            && config_.lot_size.raw() > 0 && config_.account_id[0] != '\0';
    }

    [[nodiscard]] std::expected<ClientOrderId, PaperSessionError>
    submit(const oms::OrderIntent& intent, Timestamp now) {
        if (!valid()) return std::unexpected(PaperSessionError::BadConfig);
        const auto id = venue_.submit(intent, config_.lot_size, now);
        if (!id) return std::unexpected(PaperSessionError::SubmitRefused);
        sides_.push_back({*id, intent.side});
        journal_.push_back(PaperRecord{PaperRecordKind::Submit, intent, {}, now});
        return *id;
    }

    [[nodiscard]] std::expected<int, PaperSessionError>
    on_quote(const oms::PaperQuote& quote, Timestamp now) {
        if (!valid()) return std::unexpected(PaperSessionError::BadConfig);
        std::vector<oms::PaperFill> fills;
        const auto count = venue_.on_quote(quote, now, fills);
        if (!count) return std::unexpected(PaperSessionError::QuoteRefused);
        for (const auto& fill : fills) apply_fill(fill);
        journal_.push_back(PaperRecord{PaperRecordKind::Quote, {}, quote, now});
        return *count;
    }

    [[nodiscard]] const std::vector<PaperRecord>& journal() const noexcept {
        return journal_;
    }

    [[nodiscard]] std::expected<broker_view::AccountSnapshot, PaperSessionError>
    snapshot(broker_view::EvidenceWindow observed, Price mark) const {
        if (!valid() || !broker_view::fresh(observed, observed.observed_at))
            return std::unexpected(PaperSessionError::BadConfig);
        broker_view::AccountSnapshot result{};
        result.account_session = config_.session;
        result.observed = observed;
        std::memcpy(result.account_id.data(), config_.account_id.data(),
                    result.account_id.size());
        result.profile.status = broker_view::SnapshotSectionStatus::Present;
        result.funds.status = broker_view::SnapshotSectionStatus::Present;
        result.positions.status = broker_view::SnapshotSectionStatus::Present;
        result.holdings.status = broker_view::SnapshotSectionStatus::Present;
        result.orders.status = broker_view::SnapshotSectionStatus::Present;
        result.typed_funds.cash = venue_.cash();
        result.typed_funds.available_trading_balance = venue_.cash();
        result.typed_positions_present = true;
        result.typed_positions.account_session = config_.session;
        result.typed_positions.observed = observed;
        if (!venue_.position().is_zero()) {
            result.typed_positions.count = 1;
            auto& position = result.typed_positions.position[0];
            position.instrument = config_.instrument;
            position.product = config_.product;
            position.net_qty = venue_.position();
            position.average_price = average_;
            if (mark.raw() > 0) position.last_mark = mark;
            position.marked = observed;
        }
        return result;
    }

    [[nodiscard]] static std::expected<PaperSession, PaperSessionError>
    recover(PaperSessionConfig config, const std::vector<PaperRecord>& records) {
        PaperSession result{std::move(config)};
        if (!result.valid()) return std::unexpected(PaperSessionError::BadConfig);
        for (const PaperRecord& record : records) {
            if (record.kind == PaperRecordKind::Submit) {
                const auto id = result.venue_.submit(
                    record.intent, result.config_.lot_size, record.now);
                if (!id) return std::unexpected(PaperSessionError::RecoveryMismatch);
                result.sides_.push_back({*id, record.intent.side});
            } else {
                std::vector<oms::PaperFill> fills;
                const auto count = result.venue_.on_quote(record.quote, record.now, fills);
                if (!count) return std::unexpected(PaperSessionError::RecoveryMismatch);
                for (const auto& fill : fills) result.apply_fill(fill);
            }
            result.journal_.push_back(record);
        }
        if (!result.venue_.conservation_ok())
            return std::unexpected(PaperSessionError::RecoveryMismatch);
        return result;
    }

    [[nodiscard]] Notional cash() const noexcept { return venue_.cash(); }
    [[nodiscard]] Qty position() const noexcept { return venue_.position(); }
    [[nodiscard]] Price average_price() const noexcept { return average_; }
    [[nodiscard]] bool conservation_ok() const noexcept {
        return venue_.conservation_ok();
    }

private:
    struct SideByOrder { ClientOrderId id{}; oms::IntentSide side{oms::IntentSide::Buy}; };

    [[nodiscard]] oms::IntentSide side_of(const ClientOrderId& id) const noexcept {
        for (const auto& entry : sides_) if (entry.id == id) return entry.side;
        return oms::IntentSide::Buy; // unreachable for a fill emitted by this venue.
    }

    void apply_fill(const oms::PaperFill& fill) noexcept {
        const Qty before = tracked_position_;
        const bool buy = side_of(fill.id) == oms::IntentSide::Buy;
        const std::int64_t signed_fill = buy ? fill.fill_qty.raw() : -fill.fill_qty.raw();
        const Qty after{before.raw() + signed_fill};
        const bool same_direction = before.is_zero()
            || (before.raw() > 0 && signed_fill > 0)
            || (before.raw() < 0 && signed_fill < 0);
        if (same_direction) {
            const std::int64_t old_abs = before.raw() < 0 ? -before.raw() : before.raw();
            const std::int64_t add_abs = signed_fill < 0 ? -signed_fill : signed_fill;
            const bool qty_overflow =
                old_abs > std::numeric_limits<std::int64_t>::max() - add_abs;
            const std::int64_t new_abs = qty_overflow ? 0 : old_abs + add_abs;
            const auto old_notional = notional_of(average_, Qty{old_abs});
            const auto add_notional = notional_of(fill.fill_price, Qty{add_abs});
            ExactSignedSum numerator;
            if (old_notional) numerator.add(old_notional->raw());
            if (add_notional) numerator.add(add_notional->raw());
            std::int64_t exact = 0;
            if (!qty_overflow && new_abs != 0 && old_notional && add_notional
                && numerator.try_value(exact)) {
                average_ = Price{exact / new_abs};
            } else {
                // PaperVenue has already accepted the bounded fill. Preserve a
                // deterministic, non-overflowing mark rather than invoking UB
                // while deriving display-only average-entry metadata.
                average_ = fill.fill_price;
            }
        } else if (after.is_zero()) {
            average_ = Price{0};
        } else if ((before.raw() > 0) != (after.raw() > 0)) {
            average_ = fill.fill_price; // crossed through flat; residual opened at this fill.
        }
        tracked_position_ = after;
    }

    PaperSessionConfig config_;
    oms::PaperVenue venue_;
    std::vector<SideByOrder> sides_;
    std::vector<PaperRecord> journal_;
    Qty tracked_position_{};
    Price average_{};
};

} // namespace altair::app
