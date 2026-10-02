// desktop/combined_account.hpp -- FYERS and Zerodha in one book.
//
// Each broker's read-only snapshot (data/fyers_account.json from
// altair_fyers_account, data/kite_account.json from altair_kite_account) has
// its own tab with its own shapes. A trader holding positions at both wants
// one answer first: what is held, where, and what it is doing. This reads both
// files, maps each into the same rows and says, per broker, why a section is
// missing rather than showing it as empty.
//
// READ ONLY. Nothing here talks to a broker; it reads what the helpers wrote.

#pragma once

#include "account_widgets.hpp"

#include <QDateTime>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace altair::ui {

struct CombinedFunds {
    QString broker;
    QString account;
    bool present = false;      ///< funds were fetched and parsed
    double available = 0.0;
    double used = 0.0;
    double total = 0.0;
    qint64 age_s = -1;         ///< snapshot age, -1 when unknown
    QString why_absent;
};

struct CombinedPosition {
    QString broker;
    QString symbol;
    QString product;
    qint64 qty = 0;
    double avg = 0.0;
    double ltp = 0.0;
    double pnl = 0.0;
};

struct CombinedAccount {
    std::vector<CombinedFunds> funds;
    std::vector<CombinedPosition> positions;
    QStringList notes;   ///< per-broker reasons a section is absent

    [[nodiscard]] double total_pnl() const {
        double s = 0.0;
        for (const auto& p : positions) s += p.pnl;
        return s;
    }
    [[nodiscard]] double total_available() const {
        double s = 0.0;
        for (const auto& f : funds) s += f.present ? f.available : 0.0;
        return s;
    }
};

namespace combined_detail {

[[nodiscard]] inline double num(const QJsonValue& v) {
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) return v.toString().toDouble();
    return 0.0;
}

[[nodiscard]] inline bool section_ok(const QJsonObject& root, const QString& name) {
    // The helpers write <name>_status (HTTP) and <name>_state ("present").
    // Older Kite files carry only the status.
    const int status = root.value(name + QStringLiteral("_status")).toInt();
    const QString state = root.value(name + QStringLiteral("_state")).toString();
    return status == 200 && (state.isEmpty() || state == QStringLiteral("present"));
}

[[nodiscard]] inline bool read_json(const QString& path, QJsonObject& out, QString& why) {
    QFile f(path);
    if (!f.exists()) { why = QStringLiteral("no snapshot yet"); return false; }
    if (!f.open(QIODevice::ReadOnly)) { why = QStringLiteral("snapshot unreadable"); return false; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(f.read(8 * 1024 * 1024), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        why = QStringLiteral("snapshot is not valid JSON");
        return false;
    }
    out = doc.object();
    return true;
}

[[nodiscard]] inline qint64 age_of(const QJsonObject& root, const QDateTime& now) {
    const auto at = static_cast<qint64>(root.value(QStringLiteral("fetched_at_unix")).toDouble());
    if (at <= 0) return -1;
    const qint64 age = QDateTime::fromSecsSinceEpoch(at, QTimeZone::UTC).secsTo(now);
    return age >= 0 ? age : -1;
}

/// FYERS: funds.fund_limit[] rows by title; positions.netPositions[].
inline void read_fyers(const QString& path, const QDateTime& now, CombinedAccount& out) {
    CombinedFunds f;
    f.broker = QStringLiteral("FYERS");
    QJsonObject root;
    QString why;
    if (!read_json(path, root, why)) {
        f.why_absent = why;
        out.funds.push_back(f);
        out.notes << QStringLiteral("FYERS: %1").arg(why);
        return;
    }
    f.account = root.value(QStringLiteral("account_id")).toString();
    f.age_s = age_of(root, now);
    if (section_ok(root, QStringLiteral("funds"))) {
        const QJsonArray limits = root.value(QStringLiteral("funds")).toObject()
                                      .value(QStringLiteral("fund_limit")).toArray();
        for (const QJsonValue& v : limits) {
            const QJsonObject r = v.toObject();
            const QString title = r.value(QStringLiteral("title")).toString().toLower();
            const double eq = num(r.value(QStringLiteral("equityAmount")));
            if (title.contains(QStringLiteral("available"))) { f.available = eq; f.present = true; }
            else if (title.contains(QStringLiteral("utilized")) || title.contains(QStringLiteral("utilised"))) f.used = eq;
            else if (title.contains(QStringLiteral("total balance"))) f.total = eq;
        }
        if (!f.present) f.why_absent = QStringLiteral("no Available Balance row");
    } else {
        f.why_absent = QStringLiteral("funds not fetched");
    }
    out.funds.push_back(f);

    if (!section_ok(root, QStringLiteral("positions"))) {
        out.notes << QStringLiteral("FYERS: positions not fetched (absent, not flat)");
        return;
    }
    const QJsonObject pos = root.value(QStringLiteral("positions")).toObject();
    QJsonArray net = pos.value(QStringLiteral("netPositions")).toArray();
    if (net.isEmpty())
        net = pos.value(QStringLiteral("data")).toObject().value(QStringLiteral("netPositions")).toArray();
    for (const QJsonValue& v : net) {
        const QJsonObject r = v.toObject();
        CombinedPosition p;
        p.broker = QStringLiteral("FYERS");
        p.symbol = r.value(QStringLiteral("symbol")).toString();
        p.product = r.value(QStringLiteral("productType")).toString();
        p.qty = static_cast<qint64>(num(r.value(QStringLiteral("netQty"))));
        p.avg = num(r.value(QStringLiteral("netAvg")));
        p.ltp = num(r.value(QStringLiteral("ltp")));
        p.pnl = r.contains(QStringLiteral("pl")) ? num(r.value(QStringLiteral("pl")))
                : num(r.value(QStringLiteral("realized_profit"))) + num(r.value(QStringLiteral("unrealized_profit")));
        if (!p.symbol.isEmpty()) out.positions.push_back(p);
    }
}

/// Kite: margins.data.equity; positions.data.net[].
inline void read_kite(const QString& path, const QDateTime& now, CombinedAccount& out) {
    CombinedFunds f;
    f.broker = QStringLiteral("Zerodha");
    QJsonObject root;
    QString why;
    if (!read_json(path, root, why)) {
        f.why_absent = why;
        out.funds.push_back(f);
        out.notes << QStringLiteral("Zerodha: %1").arg(why);
        return;
    }
    f.account = root.value(QStringLiteral("user_id")).toString();
    if (f.account.isEmpty()) f.account = root.value(QStringLiteral("account_id")).toString();
    f.age_s = age_of(root, now);
    if (section_ok(root, QStringLiteral("margins"))) {
        const QJsonObject eq = root.value(QStringLiteral("margins")).toObject()
                                   .value(QStringLiteral("data")).toObject()
                                   .value(QStringLiteral("equity")).toObject();
        if (!eq.isEmpty()) {
            const QJsonObject avail = eq.value(QStringLiteral("available")).toObject();
            const QJsonObject used = eq.value(QStringLiteral("utilised")).toObject();
            f.available = eq.contains(QStringLiteral("net")) ? num(eq.value(QStringLiteral("net")))
                                                             : num(avail.value(QStringLiteral("live_balance")));
            f.used = num(used.value(QStringLiteral("debits")));
            f.total = f.available + f.used;
            f.present = true;
        } else {
            f.why_absent = QStringLiteral("no equity segment");
        }
    } else {
        f.why_absent = QStringLiteral("margins not fetched");
    }
    out.funds.push_back(f);

    if (!section_ok(root, QStringLiteral("positions"))) {
        out.notes << QStringLiteral("Zerodha: positions not fetched (absent, not flat)");
        return;
    }
    const QJsonArray net = root.value(QStringLiteral("positions")).toObject()
                               .value(QStringLiteral("data")).toObject()
                               .value(QStringLiteral("net")).toArray();
    for (const QJsonValue& v : net) {
        const QJsonObject r = v.toObject();
        CombinedPosition p;
        p.broker = QStringLiteral("Zerodha");
        p.symbol = r.value(QStringLiteral("tradingsymbol")).toString();
        p.product = r.value(QStringLiteral("product")).toString();
        p.qty = static_cast<qint64>(num(r.value(QStringLiteral("quantity"))));
        p.avg = num(r.value(QStringLiteral("average_price")));
        p.ltp = num(r.value(QStringLiteral("last_price")));
        p.pnl = num(r.value(QStringLiteral("pnl")));
        if (!p.symbol.isEmpty()) out.positions.push_back(p);
    }
}

} // namespace combined_detail

[[nodiscard]] inline CombinedAccount read_combined_account(const QString& fyers_path,
                                                          const QString& kite_path,
                                                          const QDateTime& now_utc) {
    CombinedAccount out;
    combined_detail::read_fyers(fyers_path, now_utc, out);
    combined_detail::read_kite(kite_path, now_utc, out);
    return out;
}

/// Both brokers' funds and net positions, one table each.
class CombinedAccountView final : public QWidget {
public:
    CombinedAccountView(QString fyers_path, QString kite_path, QWidget* parent = nullptr)
        : QWidget(parent), fyers_path_(std::move(fyers_path)), kite_path_(std::move(kite_path)) {
        setObjectName(QStringLiteral("combinedAccount"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(6);
        head_ = new QLabel(this);
        head_->setObjectName(QStringLiteral("brokerMetricValue"));
        v->addWidget(head_);
        funds_ = account_table({QStringLiteral("BROKER"), QStringLiteral("ACCOUNT"),
                                QStringLiteral("AVAILABLE"), QStringLiteral("USED"),
                                QStringLiteral("TOTAL"), QStringLiteral("SNAPSHOT")}, this);
        funds_->setObjectName(QStringLiteral("combinedFunds"));
        funds_->setMaximumHeight(96);
        v->addWidget(funds_);
        positions_ = account_table({QStringLiteral("BROKER"), QStringLiteral("SYMBOL"),
                                    QStringLiteral("PRODUCT"), QStringLiteral("NET QTY"),
                                    QStringLiteral("AVG"), QStringLiteral("LTP"),
                                    QStringLiteral("P&L")}, this);
        positions_->setObjectName(QStringLiteral("combinedPositions"));
        v->addWidget(positions_, 1);
        notes_ = new QLabel(this);
        notes_->setWordWrap(true);
        notes_->setObjectName(QStringLiteral("brokerCardScope"));
        v->addWidget(notes_);
        refresh();
    }

    void refresh() {
        const CombinedAccount a = read_combined_account(fyers_path_, kite_path_,
                                                        QDateTime::currentDateTimeUtc());
        const QLocale loc(QLocale::English, QLocale::India);
        const auto money = [&loc](double x) { return loc.toString(x, 'f', 2); };
        funds_->setSortingEnabled(false);
        funds_->setRowCount(0);
        for (const auto& f : a.funds) {
            const int r = funds_->rowCount();
            funds_->insertRow(r);
            account_cell(funds_, r, 0, f.broker);
            account_cell(funds_, r, 1, f.account.isEmpty() ? QStringLiteral("—") : f.account);
            account_cell(funds_, r, 2, f.present ? money(f.available) : f.why_absent, f.present);
            account_cell(funds_, r, 3, f.present ? money(f.used) : QStringLiteral("—"), f.present);
            account_cell(funds_, r, 4, f.present ? money(f.total) : QStringLiteral("—"), f.present);
            account_cell(funds_, r, 5, f.age_s < 0 ? QStringLiteral("—")
                                       : f.age_s < 120 ? QStringLiteral("%1 s ago").arg(f.age_s)
                                       : f.age_s < 7200 ? QStringLiteral("%1 min ago").arg(f.age_s / 60)
                                                        : QStringLiteral("%1 h ago").arg(f.age_s / 3600));
        }
        funds_->setSortingEnabled(true);
        positions_->setSortingEnabled(false);
        positions_->setRowCount(0);
        for (const auto& p : a.positions) {
            const int r = positions_->rowCount();
            positions_->insertRow(r);
            account_cell(positions_, r, 0, p.broker);
            account_cell(positions_, r, 1, p.symbol);
            account_cell(positions_, r, 2, p.product);
            account_cell(positions_, r, 3, QString::number(p.qty), true);
            account_cell(positions_, r, 4, money(p.avg), true);
            account_cell(positions_, r, 5, money(p.ltp), true);
            account_cell(positions_, r, 6, money(p.pnl), true, p.pnl, true);
        }
        positions_->setSortingEnabled(true);
        head_->setText(QStringLiteral("COMBINED  ·  available %1  ·  %2 position(s)  ·  P&L %3")
                           .arg(money(a.total_available()))
                           .arg(a.positions.size())
                           .arg(money(a.total_pnl())));
        notes_->setText(a.notes.join(QStringLiteral("   ·   ")));
        notes_->setVisible(!a.notes.isEmpty());
    }

private:
    QString fyers_path_;
    QString kite_path_;
    QLabel* head_ = nullptr;
    QTableWidget* funds_ = nullptr;
    QTableWidget* positions_ = nullptr;
    QLabel* notes_ = nullptr;
};

} // namespace altair::ui
