// desktop/tick_model.hpp -- a Qt table model that survives a re-sort.
//
// P11Q-01.
//
// QAbstractItemModel IS INDEX-ADDRESSED, AND THAT IS THE HAZARD.
//
// P11-03 measured this in the web client: 512 patches applied against indices
// captured one re-sort earlier put 511 of 512 on the WRONG INSTRUMENT. One
// landed correctly by chance, which is what let it survive a spot check.
//
// Qt makes the mistake easier, not harder. `QModelIndex::row()` is right there
// in every handler, `QSortFilterProxyModel` adds a second index space on top,
// and the natural way to write an update is `setData(index(row, col), v)`.
// Every one of those is an index, and an index is a position in a VIEW -- one
// sort of one filter at one instant -- not an address.
//
// So this model is keyed by `InstrumentId`. `#order` holds ids in display
// order and is the only thing a row number touches; `#rows` is a map from id
// to values and is the only thing an update touches. There is deliberately no
// `updateRow(int)`. `applyTick` takes a tick, reads the id off it, and finds
// the row -- so a sort landing between the tick arriving and the tick being
// applied cannot move the target.
//
// ABSENCE IS NOT ZERO, AT THE LAST SURFACE BEFORE A HUMAN.
//
// `data()` returns an INVALID QVariant for a value that has not arrived, which
// Qt renders as an empty cell. The tempting alternative is `QVariant(0)`,
// which renders `0.00` in a P&L column -- a claim that the position is flat,
// made by the renderer rather than by the market.
//
// MONEY IS int64 PAISE UNTIL THE MOMENT IT IS DRAWN.
//
// Rule 3. `Price` crosses no boundary as a double, and the only division by
// 100 in this file is inside the formatter. A `QVariant` holding a double is
// one careless `toDouble()` from a ledger that disagrees with the broker's
// contract note by a paisa, and P12-04 reconciles to the paisa.
//
// AND THE SIGN IS IN THE TEXT.
//
// P11-07. Roughly one man in twelve cannot distinguish the red cell from the
// green one, and those two cells are the difference between a good day and a
// margin call. The `+`/`-` is always present; colour is redundant with it,
// never a substitute for it.

#pragma once

#include <core/types/units.hpp>
#include <feed/replay.hpp>

#include "format.hpp"

#include <QAbstractTableModel>
#include <QBrush>
#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>

namespace altair::ui {

/// One instrument's live state. Every money field is integer paise.
struct Row {
    std::uint32_t token = 0;
    QString symbol;

    bool has_last = false;
    std::int64_t last_paise = 0;

    bool has_open = false;
    std::int64_t open_paise = 0;

    std::int64_t volume = 0;
    std::uint64_t last_seqno = 0;
    std::int64_t last_ts_ns = 0;

    /// Ticks seen. A count, so "no data yet" is distinguishable from "zero".
    std::uint64_t ticks = 0;

    /// Does this instrument's feed report a traded quantity AT ALL?
    ///
    /// FALSE FOR AN INDEX, which does not trade. NIFTY 50 and India VIX have
    /// no turnover to publish, so their bars carry an empty volume field and
    /// the grid must draw NOTHING -- rendering 0 would claim no trading in a
    /// minute the market was open. That is the same absence-versus-zero
    /// confusion P11Q-06 unpicked in the loader and P2-12d refused to write
    /// into a CSV, and it reappeared here the moment real instruments reached
    /// the grid: the first version of P11Q-09 showed "0" against NIFTY 50.
    ///
    /// A property of the INSTRUMENT, set once when it is added, exactly like
    /// `zero_volume_is_absent` is a property of the SOURCE in bar_csv.hpp.
    bool reports_volume = true;

    /// Change against the session open, in paise. Only meaningful once both
    /// are present -- which is why both carry a `has_` flag rather than
    /// relying on a sentinel value.
    [[nodiscard]] bool has_change() const noexcept {
        return has_last && has_open;
    }
    [[nodiscard]] std::int64_t change_paise() const noexcept {
        return last_paise - open_paise;
    }
};

class TickModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        ColSymbol = 0,
        ColLast,
        ColChange,
        ColVolume,
        ColSeqno,
        ColumnCount
    };

    /// Roles beyond Qt's own.
    ///
    /// SORTING ON THE DISPLAY STRING IS WRONG, AND WRONG QUIETLY.
    ///
    /// `Qt::DisplayRole` for the LTP column is "1,23,456.78". Sort a column of
    /// those and you get a LEXICOGRAPHIC order: "1,23,456.78" sorts before
    /// "9.50", because '1' < '9'. Every grouping comma and every sign makes it
    /// worse, and the result is a plausible-looking descending list with the
    /// largest position somewhere in the middle.
    ///
    /// So the proxy sorts on `SortRole`, which returns the RAW int64 paise --
    /// the same value the ledger holds -- and never the string a human reads.
    /// `FilterRole` does the same for filter predicates, for the same reason.
    enum Role {
        SortRole = Qt::UserRole + 1,
        FilterRole,
        /// The token. What identity-addressed code asks for after a proxy has
        /// permuted the rows out from under it.
        TokenRole,
        /// True when this cell has no value. A filter needs to distinguish
        /// "blank" from "zero" and cannot do it from the display string.
        BlankRole
    };

    explicit TickModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    /// Register an instrument. Rows are created here and never by a tick --
    /// a tick for an unknown instrument is a spec-store disagreement, and
    /// inventing a row for it would hide exactly the thing rule 9 wants loud.
    void add_instrument(std::uint32_t token, const QString& symbol,
                        bool reports_volume = true) {
        if (index_of_.contains(token)) {
            return;
        }
        beginInsertRows(QModelIndex(), rows_.size(), rows_.size());
        Row r;
        r.token = token;
        r.symbol = symbol;
        r.reports_volume = reports_volume;
        index_of_.insert(token, rows_.size());
        rows_.push_back(r);
        endInsertRows();
    }

    /// Apply one tick, ADDRESSED BY THE TOKEN ON THE TICK.
    ///
    /// There is deliberately no `apply_at(int row, ...)`. A function taking a
    /// row would be correct in a static table and silently wrong the moment
    /// anything sorts -- which is every second, in a grid sorted by a live
    /// number.
    bool apply_tick(const ReplayTick& t) {
        const auto it = index_of_.constFind(t.token);
        if (it == index_of_.constEnd()) {
            ++unknown_ticks_;    // reported, never silently absorbed
            return false;
        }
        const int row = *it;
        Row& r = rows_[row];

        if (!r.has_open) {
            r.open_paise = t.last.raw();
            r.has_open = true;
        }
        r.last_paise = t.last.raw();
        r.has_last = true;
        r.volume += t.qty.raw();
        r.last_seqno = t.seqno;
        r.last_ts_ns = t.ts.ns_since_epoch();
        ++r.ticks;

        Q_EMIT dataChanged(index(row, ColLast), index(row, ColSeqno));
        return true;
    }

    [[nodiscard]] std::uint64_t unknown_ticks() const noexcept {
        return unknown_ticks_;
    }

    /// Clear every value back to "no tick yet", keeping the instruments.
    ///
    /// What a backward scrub needs. `Replayer` has no rewind -- deliberately,
    /// it is forward-only for the same reason it has no `peek()` -- so seeking
    /// backwards means replaying forward from the start through the SAME code
    /// path. That is slower and it is the only version that cannot show a
    /// number the replay has not reached yet.
    ///
    /// Note what this does NOT do: set values to zero. A reset row is a row
    /// with no data, and `has_last` going false is the difference between an
    /// empty cell and a claim that the price is 0.00.
    void reset_values() {
        if (rows_.isEmpty()) {
            return;
        }
        beginResetModel();
        for (Row& r : rows_) {
            const std::uint32_t token = r.token;
            const QString symbol = r.symbol;
            r = Row{};
            r.token = token;
            r.symbol = symbol;
        }
        unknown_ticks_ = 0;
        endResetModel();
    }

    /// Distinct display values in a column, and how many rows are blank.
    /// Feeds the Excel-style filter menu, which needs both.
    void column_values(int column, QStringList& out, int& blanks) const {
        out.clear();
        blanks = 0;
        QSet<QString> seen;
        for (int r = 0; r < rows_.size(); ++r) {
            const QModelIndex idx = index(r, column);
            if (idx.data(BlankRole).toBool()) {
                ++blanks;
                continue;
            }
            const QString v = idx.data(Qt::DisplayRole).toString();
            if (!seen.contains(v)) {
                seen.insert(v);
                out.push_back(v);
            }
        }
        out.sort();
    }

    [[nodiscard]] const Row* row_at(int i) const {
        return (i >= 0 && i < rows_.size()) ? &rows_[i] : nullptr;
    }

    // ── QAbstractTableModel ─────────────────────────────────────────────────

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : rows_.size();
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant data(const QModelIndex& idx,
                  int role = Qt::DisplayRole) const override {
        if (!idx.isValid() || idx.row() >= rows_.size()) {
            return {};
        }
        const Row& r = rows_[idx.row()];

        switch (role) {
        case Qt::DisplayRole:
            switch (idx.column()) {
            case ColSymbol:
                return r.symbol;
            case ColLast:
                // An INVALID QVariant, not 0. Qt draws an empty cell, which
                // is the honest rendering of "no tick has arrived".
                return r.has_last ? format_paise(r.last_paise) : QVariant();
            case ColChange:
                return r.has_change()
                         ? format_paise(r.change_paise(), /*explicit_sign=*/true)
                         : QVariant();
            case ColVolume:
                return (r.ticks > 0 && r.reports_volume)
                           ? QString::number(r.volume)
                           : QVariant();
            case ColSeqno:
                return r.ticks > 0 ? QString::number(r.last_seqno) : QVariant();
            default:
                return {};
            }

        case TokenRole:
            return QVariant::fromValue(r.token);

        case BlankRole:
            // A filter must tell "no tick yet" from "zero", and cannot do it
            // from the display string, which is empty for both if you are
            // careless. This is P11-04's IsAbsent operator, as a role.
            switch (idx.column()) {
            case ColSymbol: return false;
            case ColLast:   return !r.has_last;
            case ColChange: return !r.has_change();
            case ColVolume:
            case ColSeqno:  return r.ticks == 0;
            default:        return true;
            }

        case SortRole:
        case FilterRole:
            // RAW values. int64 paise for money, never the formatted string.
            switch (idx.column()) {
            case ColSymbol:
                return r.symbol;
            case ColLast:
                return r.has_last ? QVariant(qlonglong(r.last_paise))
                                  : QVariant();
            case ColChange:
                return r.has_change() ? QVariant(qlonglong(r.change_paise()))
                                      : QVariant();
            case ColVolume:
                return (r.ticks > 0 && r.reports_volume)
                           ? QVariant(qlonglong(r.volume))
                           : QVariant();
            case ColSeqno:
                return r.ticks > 0 ? QVariant(qulonglong(r.last_seqno))
                                   : QVariant();
            default:
                return {};
            }

        case Qt::TextAlignmentRole:
            return idx.column() == ColSymbol
                     ? QVariant(int(Qt::AlignLeft | Qt::AlignVCenter))
                     : QVariant(int(Qt::AlignRight | Qt::AlignVCenter));

        case Qt::ForegroundRole: {
            // Colour is REDUNDANT with the sign character, never a substitute.
            if (idx.column() != ColChange || !r.has_change()) {
                return {};
            }
            const std::int64_t c = r.change_paise();
            if (c > 0) return QBrush(QColor(0x1B, 0x8A, 0x4B));
            if (c < 0) return QBrush(QColor(0xC0, 0x39, 0x2B));
            return {};
        }

        default:
            return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
            return {};
        }
        switch (section) {
        case ColSymbol: return QStringLiteral("Symbol");
        // The unit is IN THE HEADER. "24,000.00" is ambiguous between rupees
        // and paise to anyone who did not write this program.
        case ColLast:   return QStringLiteral("LTP (INR)");
        case ColChange: return QStringLiteral("Change (INR)");
        case ColVolume: return QStringLiteral("Volume");
        case ColSeqno:  return QStringLiteral("Seq");
        default:        return {};
        }
    }

    /// Read-only, deliberately. `Qt::ItemIsEditable` is absent: there is no
    /// cell in this grid that edits anything, because the UI has no
    /// order-placing vocabulary at all.
    Qt::ItemFlags flags(const QModelIndex& idx) const override {
        return idx.isValid() ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                             : Qt::NoItemFlags;
    }

private:
    QVector<Row> rows_;
    /// token -> position in `rows_`. The ONE place a row number is derived,
    /// and it is derived from an identity rather than the other way round.
    QHash<std::uint32_t, int> index_of_;
    std::uint64_t unknown_ticks_ = 0;
};

} // namespace altair::ui
