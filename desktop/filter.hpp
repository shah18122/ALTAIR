// desktop/filter.hpp -- Excel-style column filters, and the sort that is not
// alphabetical.
//
// P11Q-02.
//
// A PROXY IS A SECOND INDEX SPACE, AND THAT IS THE HAZARD IT ADDS.
//
// `QSortFilterProxyModel` is the right tool and it doubles the number of ways
// to address a row wrongly. There is now a proxy row, a source row, and the
// identity underneath; two of those three change every time a sort runs or a
// filter narrows. P11-03 measured what index-addressing costs when the order
// moves: 511 of 512 updates landed on the wrong instrument.
//
// So nothing here ever hands a caller a row number. `token_at` maps proxy row
// -> source row -> token in one step, and the panels read that. If a call site
// needs a row it is doing something it should not.
//
// SORTING ON THE DISPLAY STRING IS WRONG, AND WRONG QUIETLY.
//
// The LTP column displays "1,23,456.78". Sorted as text that comes BEFORE
// "9.50", because '1' < '9'. Every grouping comma and every minus sign makes
// it worse, and the output is a plausible descending list with the largest
// position buried in the middle -- which is exactly the sort a trader uses to
// find their biggest risk.
//
// `lessThan` therefore reads `TickModel::SortRole`, which returns raw int64
// paise. The test measures both orders and shows they differ.
//
// BLANK IS NOT ZERO, AND IT NEEDS ITS OWN CHECKBOX.
//
// P11-04, at the filter menu. A cell with no tick yet is not a cell worth
// zero, and `>= 0` must not match it -- otherwise "show me everything at or
// above break-even" silently includes every instrument that has not traded.
//
// Excel gets this right and calls it "(Blanks)". So does this: blanks are a
// separate, explicitly checkable entry, they are excluded by every numeric
// comparison, and the count of them is shown in the menu so a column that is
// 40% blank cannot be mistaken for a column that is 40% zero.
//
// A MONEY FILTER IS TYPED IN RUPEES AND COMPARED IN PAISE.
//
// The other half of P11-04. The column holds paise; the user types 1000 and
// means a thousand RUPEES. Comparing literally selects every position over ten
// rupees and looks entirely plausible. `parse_rupees_to_paise` converts once,
// with integer arithmetic, and refuses sub-paisa precision rather than
// rounding -- a rounded filter BOUND silently changes which rows match.

#pragma once

#include "tick_model.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVBoxLayout>

#include <cstdint>
#include <optional>

namespace altair::ui {

/// Rupee text to exact paise. Ported from the web client's
/// `parseRupeesToPaise`, which exists for the same reason: the float route
/// turns "1234.35" into 123434.99999999999 and then, depending on which way it
/// rounds, into a bound one paisa from the one that was typed.
[[nodiscard]] inline std::optional<std::int64_t>
parse_rupees_to_paise(const QString& text) {
    QString t = text.trimmed();
    t.remove(QLatin1Char(','));
    if (t.isEmpty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (t.startsWith(QLatin1Char('-'))) {
        negative = true;
        t.remove(0, 1);
    } else if (t.startsWith(QLatin1Char('+'))) {
        t.remove(0, 1);
    }
    const qsizetype dot = t.indexOf(QLatin1Char('.'));
    const QString whole = dot < 0 ? t : t.left(dot);
    const QString frac = dot < 0 ? QString() : t.mid(dot + 1);
    // Sub-paisa precision is REFUSED, not rounded.
    if (frac.size() > 2) {
        return std::nullopt;
    }
    bool ok_w = true;
    bool ok_f = true;
    const qlonglong w = whole.isEmpty() ? 0 : whole.toLongLong(&ok_w);
    const QString padded = frac.leftJustified(2, u'0');
    const qlonglong f = padded.isEmpty() ? 0 : padded.toLongLong(&ok_f);
    if (!ok_w || !ok_f) {
        return std::nullopt;
    }
    const std::int64_t paise = static_cast<std::int64_t>(w) * 100
                             + static_cast<std::int64_t>(f);
    return negative ? -paise : paise;
}

/// The comparisons a numeric column offers. Ordinal zero is `None`, so a
/// default-constructed condition filters nothing rather than filtering
/// everything.
enum class Cmp : std::uint8_t {
    None = 0,
    Equals,
    NotEquals,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual
};

/// One column's filter. Two independent mechanisms, ANDed:
///   - a set of allowed display values (Excel's checkbox list);
///   - a numeric condition on the RAW value.
struct ColumnFilter {
    bool value_set_active = false;
    QSet<QString> allowed;      ///< display strings that pass
    bool allow_blanks = true;   ///< "(Blanks)", explicitly

    Cmp cmp = Cmp::None;
    std::int64_t operand = 0;   ///< already in the column's STORAGE unit

    [[nodiscard]] bool active() const noexcept {
        return value_set_active || cmp != Cmp::None || !allow_blanks;
    }
};

class FilterProxy final : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit FilterProxy(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {
        setSortRole(TickModel::SortRole);
        setDynamicSortFilter(true);
    }

    void set_filter(int column, const ColumnFilter& f) {
        filters_[column] = f;
        invalidateFilter();
        Q_EMIT filters_changed();
    }

    void clear_filter(int column) {
        filters_.remove(column);
        invalidateFilter();
        Q_EMIT filters_changed();
    }

    void clear_all() {
        filters_.clear();
        invalidateFilter();
        Q_EMIT filters_changed();
    }

    [[nodiscard]] bool has_filter(int column) const {
        const auto it = filters_.constFind(column);
        return it != filters_.constEnd() && it->active();
    }

    [[nodiscard]] int active_filter_count() const {
        int n = 0;
        for (auto it = filters_.constBegin(); it != filters_.constEnd(); ++it) {
            if (it->active()) ++n;
        }
        return n;
    }

    [[nodiscard]] ColumnFilter filter_for(int column) const {
        return filters_.value(column);
    }

    /// Proxy row -> the instrument's token, in one step.
    ///
    /// THE ONLY WAY OUT OF THIS CLASS. There is deliberately no accessor
    /// returning a source row: a caller holding one would be holding an index
    /// into an order that the next sort invalidates.
    [[nodiscard]] std::uint32_t token_at(int proxy_row) const {
        const QModelIndex p = index(proxy_row, TickModel::ColSymbol);
        if (!p.isValid()) {
            return 0;
        }
        return mapToSource(p).data(TickModel::TokenRole).toUInt();
    }

Q_SIGNALS:
    void filters_changed();

protected:
    bool filterAcceptsRow(int source_row,
                          const QModelIndex& source_parent) const override {
        const QAbstractItemModel* m = sourceModel();
        if (m == nullptr) {
            return true;
        }
        for (auto it = filters_.constBegin(); it != filters_.constEnd(); ++it) {
            const int col = it.key();
            const ColumnFilter& f = it.value();
            if (!f.active()) {
                continue;
            }
            const QModelIndex idx = m->index(source_row, col, source_parent);

            const bool blank = idx.data(TickModel::BlankRole).toBool();
            if (blank) {
                // A BLANK IS REJECTED BY ANY ACTIVE COMPARISON, and the first
                // version of this got it wrong: it let `allow_blanks` wave a
                // blank past the condition, so ">= 0" returned 4 of 4 rows
                // including the instrument that had never traded.
                //
                // That is precisely P11-04's finding -- an absent cell matches
                // no comparison -- and it is also what Excel does: the
                // "(Blanks)" checkbox belongs to the VALUE LIST, not to the
                // condition. "Show me everything at or above break-even" must
                // not quietly include every symbol with no data.
                if (f.cmp != Cmp::None) {
                    return false;
                }
                if (!f.allow_blanks) {
                    return false;
                }
                continue;
            }

            if (f.value_set_active
                && !f.allowed.contains(idx.data(Qt::DisplayRole).toString())) {
                return false;
            }

            if (f.cmp != Cmp::None) {
                const QVariant raw = idx.data(TickModel::FilterRole);
                if (!raw.isValid()) {
                    return false;
                }
                const qlonglong v = raw.toLongLong();
                const qlonglong o = static_cast<qlonglong>(f.operand);
                switch (f.cmp) {
                case Cmp::Equals:         if (!(v == o)) return false; break;
                case Cmp::NotEquals:      if (!(v != o)) return false; break;
                case Cmp::Greater:        if (!(v >  o)) return false; break;
                case Cmp::GreaterOrEqual: if (!(v >= o)) return false; break;
                case Cmp::Less:           if (!(v <  o)) return false; break;
                case Cmp::LessOrEqual:    if (!(v <= o)) return false; break;
                case Cmp::None:           break;
                }
            }
        }
        return true;
    }

    /// Compare RAW values, never display strings. See the header.
    bool lessThan(const QModelIndex& a, const QModelIndex& b) const override {
        const QVariant va = a.data(TickModel::SortRole);
        const QVariant vb = b.data(TickModel::SortRole);
        // Absent sorts LAST in both directions rather than to one end, so a
        // blank does not file itself among the genuinely flat rows (P11-03).
        if (!va.isValid() && !vb.isValid()) return false;
        if (!va.isValid()) return sortOrder() == Qt::AscendingOrder;
        if (!vb.isValid()) return sortOrder() != Qt::AscendingOrder;

        if (va.typeId() == QMetaType::QString) {
            return QString::compare(va.toString(), vb.toString(),
                                    Qt::CaseInsensitive) < 0;
        }
        return va.toLongLong() < vb.toLongLong();
    }

private:
    QHash<int, ColumnFilter> filters_;
};

// ---------------------------------------------------------------------------
// The filter menu
// ---------------------------------------------------------------------------

/// Excel's column dropdown: a checkbox list of the distinct values present,
/// "(Blanks)" as its own entry with a count, and a numeric condition.
class FilterDialog final : public QDialog {
    Q_OBJECT

public:
    FilterDialog(const QString& column_label, int column, bool numeric,
                 const QStringList& distinct_values, int blank_count,
                 const ColumnFilter& current, QWidget* parent = nullptr)
        : QDialog(parent), column_(column), numeric_(numeric) {
        setWindowTitle(QStringLiteral("Filter — %1").arg(column_label));
        setMinimumWidth(300);

        auto* layout = new QVBoxLayout(this);

        if (numeric_) {
            layout->addWidget(new QLabel(QStringLiteral("Condition"), this));
            cmp_ = new QComboBox(this);
            cmp_->addItem(QStringLiteral("(none)"), int(Cmp::None));
            cmp_->addItem(QStringLiteral("="),  int(Cmp::Equals));
            cmp_->addItem(QStringLiteral("≠"),  int(Cmp::NotEquals));
            cmp_->addItem(QStringLiteral(">"),  int(Cmp::Greater));
            cmp_->addItem(QStringLiteral("≥"),  int(Cmp::GreaterOrEqual));
            cmp_->addItem(QStringLiteral("<"),  int(Cmp::Less));
            cmp_->addItem(QStringLiteral("≤"),  int(Cmp::LessOrEqual));
            cmp_->setCurrentIndex(cmp_->findData(int(current.cmp)));
            layout->addWidget(cmp_);

            operand_ = new QLineEdit(this);
            // The placeholder says the UNIT. A box that just says "1000" is
            // how a hundredfold-wider filter looks entirely reasonable.
            operand_->setPlaceholderText(
                QStringLiteral("value in rupees, e.g. 24500.50"));
            if (current.cmp != Cmp::None) {
                operand_->setText(format_paise(current.operand));
            }
            layout->addWidget(operand_);

            error_ = new QLabel(this);
            error_->setStyleSheet(QStringLiteral("color:#F85149;"));
            error_->setVisible(false);
            layout->addWidget(error_);
        }

        blanks_ = new QCheckBox(
            QStringLiteral("(Blanks) — %1 row%2")
                .arg(blank_count)
                .arg(blank_count == 1 ? QString() : QStringLiteral("s")),
            this);
        blanks_->setChecked(current.allow_blanks);
        // The count is shown so a column that is 40% blank cannot be mistaken
        // for a column that is 40% zero.
        layout->addWidget(blanks_);

        layout->addWidget(new QLabel(QStringLiteral("Values"), this));
        values_ = new QListWidget(this);
        for (const QString& v : distinct_values) {
            auto* item = new QListWidgetItem(v, values_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(!current.value_set_active
                                        || current.allowed.contains(v)
                                    ? Qt::Checked
                                    : Qt::Unchecked);
        }
        layout->addWidget(values_);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                | QDialogButtonBox::Reset,
            this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this,
                &FilterDialog::on_accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::Reset),
                &QPushButton::clicked, this, [this] {
                    result_ = ColumnFilter{};
                    accept();
                });
    }

    [[nodiscard]] int column() const noexcept { return column_; }
    [[nodiscard]] ColumnFilter result() const noexcept { return result_; }

private Q_SLOTS:
    void on_accept() {
        ColumnFilter f;
        f.allow_blanks = blanks_->isChecked();

        if (numeric_ && cmp_ != nullptr) {
            const auto c = static_cast<Cmp>(cmp_->currentData().toInt());
            if (c != Cmp::None) {
                const auto paise = parse_rupees_to_paise(operand_->text());
                if (!paise.has_value()) {
                    // Refused, and SAID SO. Silently ignoring an unparseable
                    // bound would apply no filter while the chip claims one.
                    error_->setText(QStringLiteral(
                        "Not a value in rupees, or finer than a paisa."));
                    error_->setVisible(true);
                    return;
                }
                f.cmp = c;
                f.operand = *paise;
            }
        }

        int checked = 0;
        for (int i = 0; i < values_->count(); ++i) {
            if (values_->item(i)->checkState() == Qt::Checked) {
                f.allowed.insert(values_->item(i)->text());
                ++checked;
            }
        }
        // Everything ticked is the same as no value filter, and saying so
        // keeps the header indicator honest.
        f.value_set_active = checked != values_->count();

        result_ = f;
        accept();
    }

private:
    int column_ = 0;
    bool numeric_ = false;
    QComboBox* cmp_ = nullptr;
    QLineEdit* operand_ = nullptr;
    QLabel* error_ = nullptr;
    QCheckBox* blanks_ = nullptr;
    QListWidget* values_ = nullptr;
    ColumnFilter result_;
};

} // namespace altair::ui
