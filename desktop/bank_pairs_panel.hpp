// desktop/bank_pairs_panel.hpp -- the bank pairs paper model on Strategies >
// Pair Trading: its record (desktop/strategy_record.hpp, every "Pair trading
// A/B" model) and the engine's decisions, one row a pair a day, from
// data/live/pair_trading/decisions.csv (live/pair_trading.hpp). Reads files;
// places nothing.
#pragma once

#include "live_market.hpp"
#include "strategy_record.hpp"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

namespace altair::ui {

inline constexpr const char* kBankPairsDecisions = "data/live/pair_trading/decisions.csv";

class BankPairsPanel final : public QWidget {
public:
    explicit BankPairsPanel(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("bankPairs"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 6, 0, 0);
        auto* bar = new QHBoxLayout;
        bar->addWidget(new QLabel(QStringLiteral("<b>Bank pairs</b> · paper · 10 pairs of 5 bank futures, decided 09:20"), this));
        bar->addStretch();
        bar->addWidget(new QLabel(QStringLiteral("Decisions"), this));
        day_ = new QComboBox(this);
        day_->setObjectName(QStringLiteral("bankPairsDay"));
        bar->addWidget(day_);
        v->addLayout(bar);
        auto* split = new QSplitter(Qt::Vertical, this);
        decisions_ = make_live_table(5, {QStringLiteral("Pair"), QStringLiteral("z"), QStringLiteral("Action"),
                                         QStringLiteral("Side A"), QStringLiteral("Why")}, split);
        decisions_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        decisions_->horizontalHeader()->setStretchLastSection(true);
        decisions_->setColumnWidth(0, 190);
        split->addWidget(decisions_);
        record_ = new StrategyRecordPanel(QStringLiteral("Pair trading"), root_, split);
        split->addWidget(record_);
        split->setStretchFactor(0, 35);
        split->setStretchFactor(1, 65);
        v->addWidget(split, 1);
        QObject::connect(day_, &QComboBox::currentIndexChanged, this, [this] { if (!filling_) fill(); });
        timer_.setInterval(5000);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
        timer_.start();
        refresh();
    }

    void refresh() {
        const QString path = root_ + QLatin1Char('/') + QLatin1String(kBankPairsDecisions);
        const QFileInfo fi(path);
        const qint64 stamp = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() + fi.size() : 0;
        if (stamp == stamp_) return;
        stamp_ = stamp;
        rows_.clear();
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            (void)in.readLine();   // day,pair,z,action,side_a,reason
            while (!in.atEnd()) {
                const QStringList c = in.readLine().split(QLatin1Char(','));
                if (c.size() >= 6) rows_.push_back(c);
            }
        }
        // The newest day first: the list starts each day afresh, every past day kept.
        QStringList days;
        for (const auto& r : rows_) if (!days.contains(r[0])) days << r[0];
        std::sort(days.begin(), days.end(), std::greater<>());
        filling_ = true;
        const QString was = day_->currentText();
        const bool newest = day_->currentIndex() <= 0;
        day_->clear();
        day_->addItems(days);
        const int at = newest ? 0 : day_->findText(was);
        day_->setCurrentIndex(at < 0 ? 0 : at);
        filling_ = false;
        fill();
    }

    [[nodiscard]] QTableWidget* decisions_table() const noexcept { return decisions_; }
    [[nodiscard]] QComboBox* day_picker() const noexcept { return day_; }
    [[nodiscard]] StrategyRecordPanel* record() const noexcept { return record_; }

private:
    void fill() {
        const QString day = day_->currentText();
        int n = 0;
        for (const auto& r : rows_) n += r[0] == day ? 1 : 0;
        decisions_->setRowCount(n);
        int i = 0;
        for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {   // newest first
            const QStringList& r = *it;
            if (r[0] != day) continue;
            const bool entry = r[3].startsWith(QLatin1String("ENTRY")) || r[3].startsWith(QLatin1String("ROLL"));
            live_set(decisions_, i, 0, r[1]);
            live_set(decisions_, i, 1, r[2]);
            live_set(decisions_, i, 2, r[3], entry ? QColor(0xF4, 0xC9, 0x5D) : QColor(0xD0, 0xD6, 0xDE));
            live_set(decisions_, i, 3, r[4]);
            live_set(decisions_, i, 4, r.mid(5).join(QLatin1Char(',')));
            for (int c : {0, 2, 4}) if (auto* x = decisions_->item(i, c)) x->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ++i;
        }
    }

    QString root_;
    QComboBox* day_ = nullptr;
    QTableWidget* decisions_ = nullptr;
    StrategyRecordPanel* record_ = nullptr;
    std::vector<QStringList> rows_;
    qint64 stamp_ = -1;
    bool filling_ = false;
    QTimer timer_;
};

} // namespace altair::ui
