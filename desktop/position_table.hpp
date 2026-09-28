// desktop/position_table.hpp
// P4-05. Open positions across broker accounts, one row per position.
// Read-side only: no order capability, no oms/, no broker/.
#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/broker_positions.hpp>
#include <core/types/broker_state.hpp>
#include <core/types/units.hpp>

#include "format.hpp"

#include <QAbstractTableModel>
#include <QDate>
#include <QString>
#include <QVariant>
#include <QVector>

#include <cstdint>
#include <cstddef>
#include <compare>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace altair::ui {

/// Where a snapshot came from. PositionSnapshot does not carry this, so the
/// caller states it. Real and paper positions are NEVER combined in one row.
enum class PositionSource : std::uint8_t { Broker, Paper };

/// One broker account, as the caller wants it labelled.
struct AccountView {
    broker_view::SessionKey session{};
    PositionSource source{PositionSource::Broker};
    /// Display text for the Account column. Never parsed.
    QString label;
};

/// What the table needs to name an instrument. Supplied by the caller, who
/// owns the spec store; this header does not include instruments/.
struct InstrumentDisplay {
    QString exchange;                 ///< e.g. "NSE"
    QString symbol;                   ///< canonical display symbol
    QString segment;                  ///< e.g. "CASH", "FUT", "OPT"
    std::optional<QDate> expiry;      ///< absent for cash
    std::optional<Price> strike;      ///< paise; options only
    QString option_type;              ///< "CE" / "PE" / empty
    std::optional<LotSize> lot_size;  ///< units per contract
};

/// Resolve a key to display fields, or nullopt if the key is unknown.
using InstrumentResolver =
    std::function<std::optional<InstrumentDisplay>(broker_view::InstrumentKey)>;

enum class ApplyError : std::uint8_t {
    SessionMismatch,    ///< account.session != snapshot.account_session
    SnapshotUnusable,   ///< !broker_view::usable(snapshot, now)
    DuplicatePosition   ///< two rows in [0, count) share a RowKey
};

/// A row's identity. Plain fields so ordering can be defaulted. Product is
/// part of identity: one account can hold the same instrument as Intraday AND
/// CarryForward, and those are two positions.
struct RowKey {
    broker_view::BrokerId broker{broker_view::BrokerId::None};
    std::uint64_t account_slot{};
    std::uint64_t generation{};
    broker_view::InstrumentKey instrument{broker_view::InstrumentKey::None};
    broker_view::PositionProduct product{broker_view::PositionProduct::Unknown};
    friend constexpr auto operator<=>(const RowKey&, const RowKey&) noexcept = default;
};

} // namespace altair::ui

Q_DECLARE_METATYPE(altair::ui::RowKey)

namespace altair::ui {

class PositionTableModel : public QAbstractTableModel {
public:
    /// Order is P4-04 §7's recommended order. ColumnCount is not a column.
    enum Column : int {
        Account, Exchange, Instrument, Expiry, Strike, Type, Product,
        NetQty, Average, LastMark, Unrealised, Freshness,
        Segment, LotSizeCol, MarkedValue,
        ColumnCount
    };

    /// Same role numbering as tick_model.hpp, so one proxy works for both.
    enum Role {
        SortRole = Qt::UserRole + 1,  ///< raw int64 (paise / units / ns), or invalid
        BlankRole,                    ///< true when the cell has no value
        KeyRole                       ///< row identity, for code a proxy has permuted
    };

    /// Construct an empty model. Units: none. Preconditions: `resolve` may be
    /// empty; in that case every instrument is rendered unresolved.
    explicit PositionTableModel(InstrumentResolver resolve,
                                QObject* parent = nullptr)
        : QAbstractTableModel(parent), resolve_(std::move(resolve)) {}

    /// Replace ONE account's rows with this snapshot, by diffing keys.
    /// Returns the number of rows now shown for that account.
    /// `now` is UTC ns and is only a parameter: this file reads no clock.
    /// Preconditions: none. Session mismatch, unusable snapshot, or duplicate
    /// RowKey is refused before mutation and leaves existing rows unchanged.
    [[nodiscard]] std::expected<int, ApplyError>
    apply(const AccountView& account,
          const broker_view::PositionSnapshot& snapshot,
          Timestamp now) {
        if (account.session != snapshot.account_session) {
            return std::unexpected(ApplyError::SessionMismatch);
        }
        if (!broker_view::usable(snapshot, now)) {
            return std::unexpected(ApplyError::SnapshotUnusable);
        }

        std::map<RowKey, bool> seen;
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            const broker_view::Position& position = snapshot.position[i];
            const RowKey key{account.session.broker, account.session.account_slot,
                             account.session.generation, position.instrument,
                             position.product};
            if (!seen.emplace(key, true).second) {
                return std::unexpected(ApplyError::DuplicatePosition);
            }
        }

        std::map<RowKey, Row> incoming;
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            const broker_view::Position& position = snapshot.position[i];
            if (broker_view::is_flat(position)) {
                continue;
            }
            const RowKey key{account.session.broker, account.session.account_slot,
                             account.session.generation, position.instrument,
                             position.product};
            Row row;
            row.account = account;
            row.position = position;
            row.display = resolve_ ? resolve_(position.instrument) : std::nullopt;
            row.freshness_now = now;
            // The all-row scan above proves every RowKey unique, including
            // keys belonging to flat rows, so this insertion cannot overwrite.
            incoming.emplace(key, std::move(row));
        }

        // Remove an earlier login before diffing the current generation. The
        // map is always mutated by identity; the vector index exists only to
        // describe the structural change to QAbstractItemModel.
        for (int i = order_.size() - 1; i >= 0; --i) {
            const RowKey key = order_[i];
            if (key.broker == account.session.broker
                && key.account_slot == account.session.account_slot
                && key.generation != account.session.generation) {
                beginRemoveRows(QModelIndex(), i, i);
                rows_.erase(key);
                order_.removeAt(i);
                endRemoveRows();
            }
        }

        // Vanished and newly-flat positions are absent from `incoming`.
        for (int i = order_.size() - 1; i >= 0; --i) {
            const RowKey key = order_[i];
            if (same_session(key, account.session) && !incoming.contains(key)) {
                beginRemoveRows(QModelIndex(), i, i);
                rows_.erase(key);
                order_.removeAt(i);
                endRemoveRows();
            }
        }

        for (auto& [key, row] : incoming) {
            auto existing = rows_.find(key);
            if (existing != rows_.end()) {
                existing->second = std::move(row);
                const int display_row = order_.indexOf(key);
                if (display_row >= 0) {
                    Q_EMIT dataChanged(index(display_row, Account),
                                       index(display_row, MarkedValue));
                }
                continue;
            }

            const int display_row = order_.size();
            beginInsertRows(QModelIndex(), display_row, display_row);
            rows_.emplace(key, std::move(row));
            order_.push_back(key);
            endInsertRows();
        }
        return static_cast<int>(incoming.size());
    }

    /// Remove every row whose session equals `session` exactly.
    /// Units: none. Preconditions: none; an unknown session is a no-op.
    void drop(const broker_view::SessionKey& session) {
        for (int i = order_.size() - 1; i >= 0; --i) {
            const RowKey key = order_[i];
            if (same_session(key, session)) {
                beginRemoveRows(QModelIndex(), i, i);
                rows_.erase(key);
                order_.removeAt(i);
                endRemoveRows();
            }
        }
    }

    /// Re-evaluate the Freshness column for every row. Changes nothing else.
    /// `now` is UTC nanoseconds. Preconditions: none.
    void refresh_freshness(Timestamp now) {
        if (order_.isEmpty()) {
            return;
        }
        for (const RowKey& key : order_) {
            rows_.at(key).freshness_now = now;
        }
        Q_EMIT dataChanged(index(0, Freshness),
                           index(order_.size() - 1, Freshness));
    }

    /// Report the P4-04 default visibility decision. Units: column index.
    /// Preconditions: none; out-of-range columns return false.
    [[nodiscard]] static constexpr bool hidden_by_default(int column) noexcept {
        return column == Segment || column == LotSizeCol || column == MarkedValue;
    }

    /// Number of top-level rows. Units: rows. Preconditions: none.
    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : order_.size();
    }

    /// Number of top-level columns. Units: columns. Preconditions: none.
    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }

    /// Return one cell role. Money SortRole values are integer paise, quantity
    /// values are units, and freshness is nanoseconds. Preconditions: none;
    /// invalid indexes or unsupported roles return an invalid QVariant.
    QVariant data(const QModelIndex& idx,
                  int role = Qt::DisplayRole) const override {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= order_.size()
            || idx.column() < 0 || idx.column() >= ColumnCount) {
            return {};
        }
        const RowKey& key = order_[idx.row()];
        const Row& row = rows_.at(key);

        if (role == KeyRole) {
            return QVariant::fromValue(key);
        }
        if (role == Qt::TextAlignmentRole) {
            if (numeric_column(idx.column())) {
                return int(Qt::AlignRight | Qt::AlignVCenter);
            }
            if (idx.column() == Type) {
                return int(Qt::AlignHCenter | Qt::AlignVCenter);
            }
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (role == BlankRole) {
            return cell_state(row, idx.column()) != CellState::Value;
        }
        if (role == SortRole) {
            return sort_value(row, key, idx.column());
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        return display_value(row, key, idx.column());
    }

    /// Return horizontal headings. Units: column index. Preconditions: none.
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case Account:     return QStringLiteral("Broker / account");
        case Exchange:    return QStringLiteral("Exchange");
        case Instrument:  return QStringLiteral("Instrument");
        case Expiry:      return QStringLiteral("Expiry");
        case Strike:      return QStringLiteral("Strike (INR)");
        case Type:        return QStringLiteral("Type");
        case Product:     return QStringLiteral("Product");
        case NetQty:      return QStringLiteral("Net qty");
        case Average:     return QStringLiteral("Average (INR)");
        case LastMark:    return QStringLiteral("Last mark (INR)");
        case Unrealised:  return QStringLiteral("Unrealised P&L (INR)");
        case Freshness:   return QStringLiteral("Freshness");
        case Segment:     return QStringLiteral("Segment");
        case LotSizeCol:  return QStringLiteral("Lot size");
        case MarkedValue: return QStringLiteral("Marked value (INR)");
        default:          return {};
        }
    }

private:
    enum class CellState : std::uint8_t { Value, Absent, Error };

    struct Row {
        AccountView account;
        broker_view::Position position;
        std::optional<InstrumentDisplay> display;
        Timestamp freshness_now{};
    };

    [[nodiscard]] static bool same_session(
        const RowKey& key, const broker_view::SessionKey& session) noexcept {
        return key.broker == session.broker
            && key.account_slot == session.account_slot
            && key.generation == session.generation;
    }

    [[nodiscard]] static bool numeric_column(int column) noexcept {
        return column == Strike || column == NetQty || column == Average
            || column == LastMark || column == Unrealised
            || column == Freshness || column == LotSizeCol
            || column == MarkedValue;
    }

    [[nodiscard]] static QString product_text(
        broker_view::PositionProduct product) {
        switch (product) {
        case broker_view::PositionProduct::Intraday:     return QStringLiteral("Intraday");
        case broker_view::PositionProduct::Delivery:     return QStringLiteral("Delivery");
        case broker_view::PositionProduct::Margin:       return QStringLiteral("Margin");
        case broker_view::PositionProduct::CarryForward: return QStringLiteral("CarryForward");
        case broker_view::PositionProduct::Unknown:      return QStringLiteral("Unknown");
        }
        return QStringLiteral("Unknown");
    }

    [[nodiscard]] static QString signed_quantity(std::int64_t quantity) {
        const QString text = QString::number(quantity);
        return quantity > 0 ? QStringLiteral("+") + text : text;
    }

    [[nodiscard]] static CellState freshness_state(const Row& row) noexcept {
        const Timestamp observed = row.position.marked.observed_at;
        if (observed <= Timestamp::epoch() || observed > row.freshness_now) {
            return CellState::Absent;
        }
        return CellState::Value;
    }

    [[nodiscard]] static std::int64_t freshness_age_ns(const Row& row) noexcept {
        const Timestamp observed = row.position.marked.observed_at;
        // Proven safe before this call: observed > epoch and observed <= now.
        // Both operands are positive, so now - observed lies in [0, now) and
        // the unchecked Timestamp subtraction cannot overflow int64.
        return to_nanos(row.freshness_now - observed);
    }

    [[nodiscard]] static CellState cell_state(const Row& row, int column) {
        switch (column) {
        case Exchange:
            return row.display && !row.display->exchange.isEmpty()
                       ? CellState::Value : CellState::Absent;
        case Expiry:
            return row.display && row.display->expiry
                       ? CellState::Value : CellState::Absent;
        case Strike:
            return row.display && row.display->strike
                       ? CellState::Value : CellState::Absent;
        case Type:
            return row.display && !row.display->option_type.isEmpty()
                       ? CellState::Value : CellState::Absent;
        case LastMark:
            return row.position.last_mark ? CellState::Value : CellState::Absent;
        case Unrealised: {
            const auto result = broker_view::unrealised(row.position);
            if (result.has_value()) return CellState::Value;
            return result.error() == broker_view::PositionError::NoMark
                       ? CellState::Absent : CellState::Error;
        }
        case Freshness:
            return freshness_state(row);
        case Segment:
            return row.display && !row.display->segment.isEmpty()
                       ? CellState::Value : CellState::Absent;
        case LotSizeCol:
            return row.display && row.display->lot_size
                       ? CellState::Value : CellState::Absent;
        case MarkedValue:
            if (!row.position.last_mark) return CellState::Absent;
            return notional_of(*row.position.last_mark, row.position.net_qty)
                       ? CellState::Value : CellState::Error;
        default:
            return CellState::Value;
        }
    }

    [[nodiscard]] static QVariant sort_value(const Row& row, const RowKey& key,
                                             int column) {
        if (cell_state(row, column) != CellState::Value) {
            return {};
        }
        switch (column) {
        case Account: return account_text(row.account);
        case Exchange: return row.display->exchange;
        case Instrument:
            return row.display ? row.display->symbol
                               : unresolved_text(key.instrument);
        case Expiry: return qlonglong(row.display->expiry->toJulianDay());
        case Strike: return qlonglong(row.display->strike->raw());
        case Type: return row.display->option_type;
        case Product: return product_text(row.position.product);
        case NetQty: return qlonglong(row.position.net_qty.raw());
        case Average: return qlonglong(row.position.average_price.raw());
        case LastMark: return qlonglong(row.position.last_mark->raw());
        case Unrealised: return qlonglong(broker_view::unrealised(row.position)->raw());
        case Freshness: return qlonglong(freshness_age_ns(row));
        case Segment: return row.display->segment;
        case LotSizeCol: return qlonglong(row.display->lot_size->raw());
        case MarkedValue:
            return qlonglong(notional_of(*row.position.last_mark,
                                         row.position.net_qty)->raw());
        default: return {};
        }
    }

    [[nodiscard]] static QVariant display_value(const Row& row, const RowKey& key,
                                                int column) {
        const CellState state = cell_state(row, column);
        if (state == CellState::Absent) {
            if (column == Instrument) {
                return unresolved_text(key.instrument);
            }
            if (column == Freshness
                && row.position.marked.observed_at > row.freshness_now) {
                return QStringLiteral("FUTURE");
            }
            return QStringLiteral("—");
        }
        if (state == CellState::Error) {
            return QStringLiteral("ERR");
        }
        switch (column) {
        case Account: return account_text(row.account);
        case Exchange: return row.display->exchange;
        case Instrument:
            return row.display ? row.display->symbol
                               : unresolved_text(key.instrument);
        case Expiry: return row.display->expiry->toString(Qt::ISODate);
        case Strike: return format_paise(row.display->strike->raw());
        case Type: return row.display->option_type;
        case Product: return product_text(row.position.product);
        case NetQty: return signed_quantity(row.position.net_qty.raw());
        case Average: return format_paise(row.position.average_price.raw());
        case LastMark: return format_paise(row.position.last_mark->raw());
        case Unrealised:
            return format_paise(broker_view::unrealised(row.position)->raw(), true);
        case Freshness: {
            QString text = QString::number(freshness_age_ns(row) / 1'000'000'000LL)
                         + QStringLiteral("s");
            if (!broker_view::fresh(row.position.marked, row.freshness_now)) {
                text += QStringLiteral(" stale");
            }
            return text;
        }
        case Segment: return row.display->segment;
        case LotSizeCol: return QString::number(row.display->lot_size->raw());
        case MarkedValue:
            return format_paise(notional_of(*row.position.last_mark,
                                            row.position.net_qty)->raw());
        default: return {};
        }
    }

    [[nodiscard]] static QString unresolved_text(
        broker_view::InstrumentKey instrument) {
        return QStringLiteral("unresolved #%1")
            .arg(static_cast<std::uint32_t>(instrument));
    }

    [[nodiscard]] static QString account_text(const AccountView& account) {
        return account.label + (account.source == PositionSource::Paper
                                  ? QStringLiteral(" · Paper")
                                  : QStringLiteral(" · Broker"));
    }

    InstrumentResolver resolve_;
    QVector<RowKey> order_;
    std::map<RowKey, Row> rows_;
};

} // namespace altair::ui