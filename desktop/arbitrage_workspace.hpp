// desktop/arbitrage_workspace.hpp
// P7-05. Read-only opportunity workspace fed by an independent scanner.
#pragma once

#include "format.hpp"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace altair::ui {

inline constexpr std::size_t kArbitrageUiCapacity = 256;

/// One already-classified observation. The scanner owns pricing and policy;
/// this DTO cannot turn a refusal into an order or recompute an edge.
struct ArbitrageViewRow {
    std::uint64_t sequence{};
    QString instrument;
    QString venues;
    QString source;
    QString quote_a;
    QString quote_b;
    std::int64_t size_a{};
    std::int64_t size_b{};
    std::int64_t quote_age_ns{};
    QString premium_discount;
    std::int64_t gross_paise{};
    std::int64_t cost_paise{};
    std::int64_t net_paise{};
    QString verdict;
    QString correlation_id;
};

/// Bounded cross-thread handoff. A producer may publish on every admitted
/// event; the UI drains at its own cadence. No UI timer controls scan cadence.
class ArbitrageInbox final {
public:
    bool publish(ArbitrageViewRow row) {
        QMutexLocker lock(&mutex_);
        if (count_ >= rows_.size()) {
            ++dropped_;
            return false; // visible refusal, never silent overwrite.
        }
        rows_[count_++] = std::move(row);
        return true;
    }

    std::size_t drain(ArbitrageViewRow* out, std::size_t capacity,
                      std::uint64_t& dropped) {
        QMutexLocker lock(&mutex_);
        dropped = dropped_;
        if (capacity < count_) return 0; // caller must provide full capacity.
        for (std::size_t i = 0; i < count_; ++i) out[i] = std::move(rows_[i]);
        const std::size_t result = count_;
        count_ = 0;
        return result;
    }

private:
    QMutex mutex_;
    std::array<ArbitrageViewRow, kArbitrageUiCapacity> rows_{};
    std::size_t count_{};
    std::uint64_t dropped_{};
};

class ArbitrageTableModel final : public QAbstractTableModel {
public:
    enum Column : int {
        Instrument, Venues, Source, QuoteA, QuoteB, SizeA, SizeB, QuoteAge,
        PremiumDiscount, Gross, Cost, Net, Verdict, Correlation, ColumnCount
    };
    enum Role { SortRole = Qt::UserRole + 1 };

    explicit ArbitrageTableModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void append(const ArbitrageViewRow* incoming, std::size_t count) {
        if (count == 0) return;
        const int first = static_cast<int>(rows_.size());
        const int last = first + static_cast<int>(count) - 1;
        beginInsertRows({}, first, last);
        for (std::size_t i = 0; i < count; ++i) rows_.push_back(incoming[i]);
        endInsertRows();
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static constexpr const char* names[] = {
            "Instrument", "Venues", "Source", "Quote A", "Quote B", "Size A",
            "Size B", "Quote age", "Premium / discount", "Gross edge (INR)",
            "Costs (INR)", "Net edge (INR)", "Verdict / refusal", "Correlation ID"};
        return section >= 0 && section < ColumnCount
            ? QString::fromUtf8(names[section]) : QVariant{};
    }
    QVariant data(const QModelIndex& idx, int role = Qt::DisplayRole) const override {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= rowCount()
            || idx.column() < 0 || idx.column() >= ColumnCount) return {};
        const auto& r = rows_[static_cast<std::size_t>(idx.row())];
        if (role == Qt::TextAlignmentRole && idx.column() >= SizeA && idx.column() <= Net)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role == SortRole) {
            switch (idx.column()) {
            case SizeA: return r.size_a;
            case SizeB: return r.size_b;
            case QuoteAge: return r.quote_age_ns;
            case Gross: return r.gross_paise;
            case Cost: return r.cost_paise;
            case Net: return r.net_paise;
            default: return data(idx, Qt::DisplayRole);
            }
        }
        if (role != Qt::DisplayRole) return {};
        switch (idx.column()) {
        case Instrument: return r.instrument;
        case Venues: return r.venues;
        case Source: return r.source;
        case QuoteA: return r.quote_a;
        case QuoteB: return r.quote_b;
        case SizeA: return QString::number(r.size_a);
        case SizeB: return QString::number(r.size_b);
        case QuoteAge: return r.quote_age_ns < 0 ? QStringLiteral("unknown")
            : QStringLiteral("%1 ms").arg(r.quote_age_ns / 1'000'000);
        case PremiumDiscount: return r.premium_discount;
        case Gross: return format_paise(r.gross_paise, true);
        case Cost: return format_paise(r.cost_paise);
        case Net: return format_paise(r.net_paise, true);
        case Verdict: return r.verdict;
        case Correlation: return r.correlation_id;
        default: return {};
        }
    }

private:
    std::vector<ArbitrageViewRow> rows_;
};

class ArbitrageWorkspace final : public QWidget {
public:
    std::function<void()> open_broker_diagnostics;

    explicit ArbitrageWorkspace(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        auto* title = new QLabel(QStringLiteral("<h3>Arbitrage — observable universe</h3>"), this);
        layout->addWidget(title);
        auto* note = new QLabel(QStringLiteral(
            "Event-driven scans run outside this refresh. Every row carries executable quotes, "
            "post-cost edge, quote age and refusal; a futures discount is not called profit."), this);
        note->setWordWrap(true);
        layout->addWidget(note);

        auto* manual = new QHBoxLayout;
        manual_a_ = new QLineEdit(QStringLiteral("NIFTY"), this);
        manual_a_->setObjectName(QStringLiteral("arbitrageLegA"));
        manual_a_->setPlaceholderText(QStringLiteral("instrument / contract A"));
        venue_a_ = new QComboBox(this);
        venue_a_->addItems({QStringLiteral("NSE CASH"), QStringLiteral("BSE CASH"),
                            QStringLiteral("NSE FUTURE")});
        manual_b_ = new QLineEdit(QStringLiteral("NIFTY"), this);
        manual_b_->setObjectName(QStringLiteral("arbitrageLegB"));
        manual_b_->setPlaceholderText(QStringLiteral("instrument / contract B"));
        venue_b_ = new QComboBox(this);
        venue_b_->addItems({QStringLiteral("NSE CASH"), QStringLiteral("BSE CASH"),
                            QStringLiteral("NSE FUTURE")});
        venue_b_->setCurrentIndex(1);
        manual->addWidget(new QLabel(QStringLiteral("Manual pair"), this));
        manual->addWidget(manual_a_, 1);
        manual->addWidget(venue_a_);
        manual->addWidget(new QLabel(QStringLiteral("↔"), this));
        manual->addWidget(manual_b_, 1);
        manual->addWidget(venue_b_);
        layout->addLayout(manual);
        manual_status_ = new QLabel(this);
        manual_status_->setWordWrap(true);
        manual_status_->setStyleSheet(QStringLiteral(
            "background:#162027;color:#B8C5CC;padding:8px;border:1px solid #263842;"));
        layout->addWidget(manual_status_);
        const auto update_manual = [this] { update_manual_status(); };
        connect(manual_a_, &QLineEdit::textChanged, this, update_manual);
        connect(manual_b_, &QLineEdit::textChanged, this, update_manual);
        connect(venue_a_, &QComboBox::currentIndexChanged, this,
                [update_manual](int) { update_manual(); });
        connect(venue_b_, &QComboBox::currentIndexChanged, this,
                [update_manual](int) { update_manual(); });
        update_manual_status();
        auto* controls = new QHBoxLayout;
        filter_ = new QLineEdit(this);
        filter_->setPlaceholderText(QStringLiteral("Filter instrument, venue, source, verdict or correlation…"));
        filter_->setClearButtonEnabled(true);
        controls->addWidget(filter_, 1);
        auto* diagnostics = new QPushButton(QStringLiteral("Broker diagnostics"), this);
        controls->addWidget(diagnostics);
        dropped_ = new QLabel(QStringLiteral("No UI handoff drops"), this);
        controls->addWidget(dropped_);
        layout->addLayout(controls);

        model_ = new ArbitrageTableModel(this);
        proxy_ = new QSortFilterProxyModel(this);
        proxy_->setSourceModel(model_);
        proxy_->setSortRole(ArbitrageTableModel::SortRole);
        proxy_->setFilterKeyColumn(-1);
        proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
        table_ = new QTableView(this);
        table_->setModel(proxy_);
        table_->setSortingEnabled(true);
        table_->setAlternatingRowColors(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setSectionsMovable(true);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_, 1);
        connect(filter_, &QLineEdit::textChanged, proxy_,
                &QSortFilterProxyModel::setFilterFixedString);
        connect(diagnostics, &QPushButton::clicked, this, [this] {
            if (open_broker_diagnostics) open_broker_diagnostics();
        });
        refresh_.setInterval(250);
        connect(&refresh_, &QTimer::timeout, this, [this] { drain(); });
        refresh_.start();
    }

    ArbitrageInbox& inbox() noexcept { return inbox_; }
    ArbitrageTableModel* model() const noexcept { return model_; }
    [[nodiscard]] bool manual_pair_valid() const {
        const QString a = manual_a_->text().trimmed().toUpper();
        const QString b = manual_b_->text().trimmed().toUpper();
        return !a.isEmpty() && !b.isEmpty()
            && !(a == b && venue_a_->currentText() == venue_b_->currentText());
    }
    [[nodiscard]] QString manual_pair_status() const {
        return manual_status_ != nullptr ? manual_status_->text() : QString{};
    }
    void set_manual_pair(const QString& a, int venue_a,
                         const QString& b, int venue_b) {
        manual_a_->setText(a);
        manual_b_->setText(b);
        if (venue_a >= 0 && venue_a < venue_a_->count()) venue_a_->setCurrentIndex(venue_a);
        if (venue_b >= 0 && venue_b < venue_b_->count()) venue_b_->setCurrentIndex(venue_b);
        update_manual_status();
    }
    void drain() {
        std::array<ArbitrageViewRow, kArbitrageUiCapacity> rows;
        std::uint64_t dropped = 0;
        const std::size_t count = inbox_.drain(rows.data(), rows.size(), dropped);
        if (count > 0) model_->append(rows.data(), count);
        dropped_->setText(dropped == 0
            ? QStringLiteral("No UI handoff drops")
            : QStringLiteral("%1 UI handoff row(s) refused").arg(dropped));
    }

private:
    void update_manual_status() {
        if (!manual_pair_valid()) {
            manual_status_->setText(QStringLiteral(
                "REFUSED — both legs must be named and an identical instrument/venue "
                "cannot be compared with itself."));
            return;
        }
        manual_status_->setText(QStringLiteral(
            "Watching <b>%1 · %2</b> against <b>%3 · %4</b>. A candidate is "
            "actionable only after both books have fresh two-sided depth, common "
            "executable size, settlement/inventory eligibility and a positive edge "
            "after the complete cost schedule. Deployment remains paper-gated until "
            "the OMS explicitly authorises both legs atomically.")
            .arg(manual_a_->text().trimmed().toUpper(), venue_a_->currentText(),
                 manual_b_->text().trimmed().toUpper(), venue_b_->currentText()));
    }

    ArbitrageInbox inbox_;
    ArbitrageTableModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QLineEdit* manual_a_ = nullptr;
    QLineEdit* manual_b_ = nullptr;
    QComboBox* venue_a_ = nullptr;
    QComboBox* venue_b_ = nullptr;
    QLabel* manual_status_ = nullptr;
    QLabel* dropped_ = nullptr;
    QTimer refresh_;
};

} // namespace altair::ui
