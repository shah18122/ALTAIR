// desktop/funds_summary.hpp
// P4-06. Provider-neutral, read-only funds summary for the Terminal.
#pragma once

#include <core/types/broker_state.hpp>

#include "format.hpp"

#include <QAbstractTableModel>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <compare>
#include <iterator>
#include <limits>
#include <map>
#include <optional>

namespace altair::ui {

enum class FundsSource : std::uint8_t { Broker, Paper };

struct FundsAccountView {
    broker_view::SessionKey session{};
    FundsSource source{FundsSource::Broker};
    QString label;
    bool connected{};
    broker_view::AccountFunds funds{};
};

struct FundsKey {
    broker_view::BrokerId broker{broker_view::BrokerId::None};
    std::uint64_t account_slot{};
    friend constexpr auto operator<=>(const FundsKey&, const FundsKey&) noexcept = default;
};

/// One row per broker account plus a deliberately qualified comparable total.
/// Applying another provider segment for the same broker/account replaces the
/// row; segment balances can therefore never be accidentally counted twice.
class FundsSummaryModel final : public QAbstractTableModel {
public:
    enum Column : int { Account, Cash, Available, Collateral, Used, State, ColumnCount };

    explicit FundsSummaryModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    bool apply(const FundsAccountView& account) {
        if (!broker_view::valid(account.session)) return false;
        const FundsKey key{account.session.broker, account.session.account_slot};
        const auto found = rows_.find(key);
        if (found == rows_.end()) {
            const int row = static_cast<int>(rows_.size());
            beginInsertRows({}, row, row);
            rows_.emplace(key, account);
            endInsertRows();
        } else {
            found->second = account; // includes generation: a relogin replaces, never duplicates.
            const int row = row_of(key);
            Q_EMIT dataChanged(index(row, 0), index(row, ColumnCount - 1));
        }
        Q_EMIT dataChanged(index(total_row(), 0), index(total_row(), ColumnCount - 1));
        return true;
    }

    void clear() {
        beginResetModel();
        rows_.clear();
        endResetModel();
    }

    [[nodiscard]] int account_count() const noexcept {
        return static_cast<int>(rows_.size());
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : account_count() + 1; // total remains visible when empty.
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case Account: return QStringLiteral("Broker / account");
        case Cash: return QStringLiteral("Cash (INR)");
        case Available: return QStringLiteral("Available (INR)");
        case Collateral: return QStringLiteral("Collateral (INR)");
        case Used: return QStringLiteral("Used margin (INR)");
        case State: return QStringLiteral("Evidence");
        default: return {};
        }
    }

    QVariant data(const QModelIndex& idx, int role = Qt::DisplayRole) const override {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= rowCount()
            || idx.column() < 0 || idx.column() >= ColumnCount) return {};
        if (role == Qt::ToolTipRole && idx.row() == total_row())
            return QStringLiteral("Comparable arithmetic only. Funds at different brokers are not fungible or transferable by Altair.");
        if (role == Qt::TextAlignmentRole && idx.column() >= Cash && idx.column() <= Used)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole) return {};

        if (idx.row() == total_row()) return total_value(idx.column());
        const FundsAccountView& row = row_at(idx.row());
        switch (idx.column()) {
        case Account:
            return row.label + (row.source == FundsSource::Paper
                ? QStringLiteral(" · PAPER") : QStringLiteral(" · BROKER"));
        case Cash: return amount(row.funds.cash);
        case Available: return amount(row.funds.available_trading_balance);
        case Collateral: return amount(row.funds.collateral);
        case Used: return amount(row.funds.utilised_margin);
        case State: return row.connected ? QStringLiteral("verified snapshot")
                                         : QStringLiteral("disconnected / cached");
        default: return {};
        }
    }

    [[nodiscard]] int total_row() const noexcept { return account_count(); }

private:
    struct Sum {
        std::int64_t value{};
        int known{};
        bool overflow{};
    };

    [[nodiscard]] int row_of(const FundsKey& wanted) const {
        int row = 0;
        for (const auto& [key, value] : rows_) {
            Q_UNUSED(value);
            if (key == wanted) return row;
            ++row;
        }
        return -1;
    }

    [[nodiscard]] const FundsAccountView& row_at(int wanted) const {
        auto it = rows_.cbegin();
        std::advance(it, wanted);
        return it->second;
    }

    [[nodiscard]] static QString amount(const std::optional<Notional>& value) {
        return value ? format_paise(value->raw()) : QStringLiteral("—");
    }

    template <typename Select>
    [[nodiscard]] Sum sum(Select select) const {
        Sum result;
        for (const auto& [key, row] : rows_) {
            Q_UNUSED(key);
            const auto value = select(row.funds);
            if (!value) continue;
            const std::int64_t v = value->raw();
            if ((v > 0 && result.value > std::numeric_limits<std::int64_t>::max() - v)
                || (v < 0 && result.value < std::numeric_limits<std::int64_t>::min() - v)) {
                result.overflow = true;
                continue;
            }
            result.value += v;
            ++result.known;
        }
        return result;
    }

    [[nodiscard]] QString total_amount(const Sum& result) const {
        if (result.overflow) return QStringLiteral("ERR");
        if (result.known == 0) return QStringLiteral("—");
        const QString value = format_paise(result.value);
        return result.known == account_count()
            ? value : value + QStringLiteral(" · INCOMPLETE");
    }

    [[nodiscard]] QVariant total_value(int column) const {
        switch (column) {
        case Account: return QStringLiteral("Comparable total · NOT FUNGIBLE");
        case Cash: return total_amount(sum([](const auto& f) { return f.cash; }));
        case Available: return total_amount(sum([](const auto& f) { return f.available_trading_balance; }));
        case Collateral: return total_amount(sum([](const auto& f) { return f.collateral; }));
        case Used: return total_amount(sum([](const auto& f) { return f.utilised_margin; }));
        case State:
            if (rows_.empty()) return QStringLiteral("no account snapshots");
            for (const auto& [key, row] : rows_) {
                Q_UNUSED(key);
                if (!row.connected) return QStringLiteral("INCOMPLETE · one or more disconnected");
            }
            return QStringLiteral("all snapshots verified");
        default: return {};
        }
    }

    std::map<FundsKey, FundsAccountView> rows_;
};

} // namespace altair::ui
