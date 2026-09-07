// desktop/kite_panel.hpp -- the Kite account panel: funds, positions,
// holdings, orders, and the full endpoint catalogue.
//
// P20-02.
//
// WHAT REPLACED "BROKER WIRING", AND WHY IT IS STILL NOT A TRADING TERMINAL.
//
// The old page listed which parts of the Kite integration were built. Useful,
// and it answered a developer's question rather than an operator's. This one
// answers "what does my account look like right now, and how do I know" -- and
// then still shows the wiring table underneath, because a number is only worth
// as much as the pipe it came through.
//
// THREE RULES IT KEEPS.
//
// 1. THE UI HAS NO NETWORK. `desktop/` may not link `broker/`; the gate 3
//    allow-list refuses it at configure time. So this panel reads a SNAPSHOT
//    written by `altair_kite_account`, and cannot itself call the API. The
//    "Refresh" button therefore tells you the command to run rather than
//    running it -- which is honest about where the boundary is instead of
//    hiding it behind a spinner that would never resolve.
//
// 2. EVERY NUMBER CARRIES ITS AGE. A funds figure with no timestamp is worse
//    than no funds figure, because it will be believed. The header line is the
//    age, in words, before any rupee amount appears.
//
// 3. ABSENT IS NOT ZERO. A snapshot where `positions` is null means "not
//    fetched"; one where it is an empty list means "no open positions". The
//    panel says which. This project has confused those two in six other
//    places and paid for it every time.
//
// AND THE BUTTONS THAT ARE NOT HERE.
//
// Kite exposes 49 endpoints. Roughly a third of them MUTATE: place, modify and
// cancel orders, GTT triggers, mutual-fund orders and SIPs, position
// conversion. None of them gets a button in this window, and that is not an
// omission to be fixed later -- CLAUDE.md is unconditional that `oms/` is the
// only thing that can place an order, and the in-process decision kept exactly
// one safety property by construction.
//
// So the catalogue below lists ALL of them, marks each as READ or MUTATE, and
// says where a mutating one has to live. A reader learns the whole surface and
// where the wall is, which is more useful than a greyed-out button.

#pragma once

#include "feed_status.hpp"

#include <QDateTime>
#include <QFile>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace altair::ui {

/// One row of the endpoint catalogue.
struct KiteEndpoint {
    const char* group;
    const char* path;
    /// True when the call CHANGES something at the broker.
    bool mutates;
    /// Where it is, or where it would have to live.
    const char* state;
};

/// Every endpoint in Zerodha's own Go client, read from
/// `research/reference/gokiteconnect/connect.go`.
///
/// Transcribed from the source rather than from the web documentation,
/// because the docs and the client have disagreed before -- P0's reference
/// note records two time formats with different semantics found the same way.
[[nodiscard]] inline std::vector<KiteEndpoint> kite_endpoints() {
    return {
        // ---- session -----------------------------------------------------
        {"Session", "/session/token", true,
         "broker/kite_login.hpp — altair_kite_login, by hand"},
        {"Session", "/session/refresh_token", true, "not built"},
        {"Session", "/session/token (invalidate)", true,
         "not built — logout is a broker-console action today"},
        // ---- user --------------------------------------------------------
        {"User", "/user/profile", false, "altair_kite_account → snapshot"},
        {"User", "/user/profile/full", false, "not built"},
        {"User", "/user/margins", false, "altair_kite_account → snapshot"},
        {"User", "/user/margins/{segment}", false, "not built"},
        // ---- portfolio ---------------------------------------------------
        {"Portfolio", "/portfolio/positions", false,
         "altair_kite_account → snapshot"},
        {"Portfolio", "/portfolio/holdings", false,
         "altair_kite_account → snapshot"},
        {"Portfolio", "/portfolio/holdings/auctions", false, "not built"},
        {"Portfolio", "/portfolio/positions (convert)", true,
         "oms/ ONLY — changes a product type, which changes margin"},
        // ---- orders ------------------------------------------------------
        {"Orders", "/orders", false, "altair_kite_account → snapshot"},
        {"Orders", "/trades", false, "not built — P12-04 reconciles notes"},
        {"Orders", "/orders/{id}", false, "not built"},
        {"Orders", "/orders/{id}/trades", false, "not built"},
        {"Orders", "/orders/{variety} (place)", true,
         "oms/kite_adapter.hpp builds the body; DOES NOT SEND"},
        {"Orders", "/orders/{variety}/{id} (modify)", true, "oms/ ONLY"},
        {"Orders", "/orders/{variety}/{id} (cancel)", true, "oms/ ONLY"},
        // ---- margins -----------------------------------------------------
        {"Margins", "/margins/orders", false,
         "not built — risk/cost.hpp computes ours independently"},
        {"Margins", "/margins/basket", false, "not built"},
        {"Margins", "/charges/orders", false,
         "not built — and it is the natural P12-04 cross-check"},
        // ---- market ------------------------------------------------------
        {"Market", "/instruments", false,
         "broker/kite_historical + instruments/kite_dump.hpp"},
        {"Market", "/instruments/{exchange}", false, "not built"},
        {"Market", "/instruments/historical/{token}/{interval}", false,
         "altair_kite_fetch — filled dataset/ with 2.8M bars"},
        {"Market", "/instruments/{ex}/{sym}/trigger_range", false,
         "not built"},
        {"Market", "/quote", false,
         "broker/kite_quote.hpp — PARSER BUILT (P20-03), 22 checks green. "
         "Fetching needs a session; parsing does not, and the parser is "
         "where the bugs are. This is what fills the watchlist bid/ask."},
        {"Market", "/quote/ltp", false, "not built"},
        {"Market", "/quote/ohlc", false, "not built"},
        // ---- GTT ---------------------------------------------------------
        {"GTT", "/gtt/triggers (list)", false, "not built"},
        {"GTT", "/gtt/triggers/{id}", false, "not built"},
        {"GTT", "/gtt/triggers (place)", true, "oms/ ONLY — a resting order"},
        {"GTT", "/gtt/triggers/{id} (modify)", true, "oms/ ONLY"},
        {"GTT", "/gtt/triggers/{id} (delete)", true, "oms/ ONLY"},
        // ---- mutual funds ------------------------------------------------
        {"Mutual funds", "/mf/holdings", false, "not built — out of scope"},
        {"Mutual funds", "/mf/orders", false, "not built — out of scope"},
        {"Mutual funds", "/mf/sips", false, "not built — out of scope"},
        {"Mutual funds", "/mf/orders (place)", true, "oms/ ONLY, if ever"},
        {"Mutual funds", "/mf/sips (place)", true, "oms/ ONLY, if ever"},
        // ---- streaming ---------------------------------------------------
        {"WebSocket", "wss://ws.kite.trade", false,
         "feed/kite_decoder.hpp DECODES it; nothing SUBSCRIBES yet"},
    };
}

/// Rupees from paise-free doubles, Indian grouping.
[[nodiscard]] inline QString rupees(double v) {
    const bool neg = v < 0.0;
    QString s = QString::number(std::fabs(v), 'f', 2);
    const int dot = s.indexOf('.');
    QString ip = s.left(dot), fp = s.mid(dot);
    // Indian grouping: last three, then twos.
    QString grouped;
    if (ip.size() > 3) {
        grouped = ip.right(3);
        ip.chop(3);
        while (ip.size() > 2) {
            grouped = ip.right(2) + "," + grouped;
            ip.chop(2);
        }
        if (!ip.isEmpty()) { grouped = ip + "," + grouped; }
    } else {
        grouped = ip;
    }
    return (neg ? QStringLiteral("-₹") : QStringLiteral("₹"))
         + grouped + fp;
}

class KitePanel : public QWidget {
public:
    explicit KitePanel(const QString& snapshot_path,
                       const QString& session_path, QWidget* parent = nullptr)
        : QWidget(parent), snapshot_(snapshot_path), session_(session_path) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);

        head_ = new QLabel;
        head_->setWordWrap(true);
        head_->setContentsMargins(14, 12, 14, 8);
        v->addWidget(head_);

        auto* refresh = new QPushButton(
            QStringLiteral("Re-read snapshot   (the UI has no network — see "
                           "the note)"));
        refresh->setContentsMargins(14, 0, 14, 0);
        connect(refresh, &QPushButton::clicked, this, [this] { reload(); });
        v->addWidget(refresh);

        tabs_ = new QTabWidget;
        funds_ = make_pane();
        positions_ = make_pane();
        holdings_ = make_pane();
        orders_ = make_pane();
        catalogue_ = make_pane();
        tabs_->addTab(funds_, QStringLiteral("Funds"));
        tabs_->addTab(positions_, QStringLiteral("Positions"));
        tabs_->addTab(holdings_, QStringLiteral("Holdings"));
        tabs_->addTab(orders_, QStringLiteral("Orders"));
        tabs_->addTab(catalogue_, QStringLiteral("API surface"));
        v->addWidget(tabs_, 1);

        catalogue_->setPlainText(catalogue_text());
        reload();
    }

private:
    [[nodiscard]] QPlainTextEdit* make_pane() {
        auto* p = new QPlainTextEdit;
        p->setReadOnly(true);
        p->setStyleSheet(QStringLiteral(
            "QPlainTextEdit{background:#11171C;color:#D6DBDF;"
            "font-family:Consolas,monospace;font-size:12px;border:none;}"));
        return p;
    }

    void reload() {
        QFile f(snapshot_);
        if (!f.exists()) {
            head_->setText(QStringLiteral(
                "<b style='color:#B9770B'>NO SNAPSHOT</b> — %1 does not "
                "exist.<br><br>"
                "The window cannot fetch it: <code>desktop/</code> may not "
                "link <code>broker/</code>, and the gate&nbsp;3 allow-list "
                "refuses it at configure time. That is the one safety "
                "property the in-process decision kept by construction — "
                "<b>the UI cannot trade</b> — and a panel that could reach "
                "the broker's HTTP surface would hold it only by good "
                "manners.<br><br>"
                "Run this, then press the button above:<br>"
                "<code>&nbsp;&nbsp;.\\build\\net\\app\\altair_kite_account.exe "
                "--go</code>").arg(snapshot_));
            for (auto* p : {funds_, positions_, holdings_, orders_}) {
                p->setPlainText(
                    QStringLiteral("Not fetched.\n\nThis is ABSENT, not "
                                   "empty — it does not mean you hold "
                                   "nothing."));
            }
            return;
        }
        f.open(QIODevice::ReadOnly);
        const QByteArray raw = f.readAll();
        f.close();

        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(raw, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            head_->setText(QStringLiteral(
                "<b style='color:#C0392B'>SNAPSHOT DID NOT PARSE</b> — %1")
                    .arg(err.errorString()));
            return;
        }
        const QJsonObject o = doc.object();

        // ---- THE AGE, BEFORE ANY NUMBER --------------------------------
        const qint64 at = static_cast<qint64>(
            o.value(QStringLiteral("fetched_at_unix")).toDouble());
        const QDateTime when = QDateTime::fromSecsSinceEpoch(at);
        const qint64 age_s =
            QDateTime::currentSecsSinceEpoch() - at;
        QString age;
        if (age_s < 90) { age = QStringLiteral("%1 seconds ago").arg(age_s); }
        else if (age_s < 5400) {
            age = QStringLiteral("%1 minutes ago").arg(age_s / 60);
        } else if (age_s < 172800) {
            age = QStringLiteral("%1 hours ago").arg(age_s / 3600);
        } else {
            age = QStringLiteral("%1 DAYS ago").arg(age_s / 86400);
        }
        const bool stale = age_s > 900;
        head_->setText(QStringLiteral(
            "Snapshot taken <b>%1</b> (%2)%3<br>"
            "<span style='color:#7F8C8D'>Every figure below is from that "
            "instant. Nothing here is live — the window has no network.</span>")
                .arg(age)
                .arg(when.toString(QStringLiteral("ddd dd MMM, HH:mm:ss")))
                .arg(stale ? QStringLiteral(
                         "  <b style='color:#B9770B'>— STALE</b>")
                           : QString()));

        render_funds(o);
        render_positions(o);
        render_holdings(o);
        render_orders(o);
    }

    /// `null` means NOT FETCHED. `[]` means genuinely empty. The panel must
    /// never render them the same way.
    [[nodiscard]] static bool fetched(const QJsonObject& o, const char* key) {
        return o.contains(QLatin1String(key))
            && !o.value(QLatin1String(key)).isNull();
    }

    void render_funds(const QJsonObject& o) {
        if (!fetched(o, "margins")) {
            funds_->setPlainText(QStringLiteral(
                "margins: NOT FETCHED (HTTP %1)\n\nAbsent, not zero.")
                    .arg(o.value(QStringLiteral("margins_status")).toInt()));
            return;
        }
        const QJsonObject d =
            o.value(QStringLiteral("margins")).toObject()
             .value(QStringLiteral("data")).toObject();
        QString s;
        for (const QString& seg : {QStringLiteral("equity"),
                                   QStringLiteral("commodity")}) {
            if (!d.contains(seg)) { continue; }
            const QJsonObject m = d.value(seg).toObject();
            const QJsonObject av =
                m.value(QStringLiteral("available")).toObject();
            const QJsonObject us =
                m.value(QStringLiteral("utilised")).toObject();
            s += QStringLiteral("%1  (%2)\n")
                     .arg(seg.toUpper())
                     .arg(m.value(QStringLiteral("enabled")).toBool()
                              ? QStringLiteral("enabled")
                              : QStringLiteral("NOT ENABLED"));
            s += QStringLiteral("  %1 %2\n\n")
                     .arg(QStringLiteral("NET"), -22)
                     .arg(rupees(m.value(QStringLiteral("net")).toDouble()), 18);
            s += QStringLiteral("  available\n");
            for (const auto& [k, label] : {
                     std::pair{"cash", "cash"},
                     {"opening_balance", "opening balance"},
                     {"live_balance", "live balance"},
                     {"collateral", "collateral"},
                     {"intraday_payin", "intraday payin"},
                     {"adhoc_margin", "adhoc margin"}}) {
                s += QStringLiteral("    %1 %2\n")
                         .arg(QLatin1String(label), -20)
                         .arg(rupees(av.value(QLatin1String(k)).toDouble()), 18);
            }
            s += QStringLiteral("\n  utilised\n");
            for (const auto& [k, label] : {
                     std::pair{"debits", "debits"},
                     {"span", "SPAN"},
                     {"exposure", "exposure"},
                     {"option_premium", "option premium"},
                     {"m2m_realised", "M2M realised"},
                     {"m2m_unrealised", "M2M unrealised"},
                     {"delivery", "delivery"},
                     {"payout", "payout"},
                     {"turnover", "turnover"}}) {
                s += QStringLiteral("    %1 %2\n")
                         .arg(QLatin1String(label), -20)
                         .arg(rupees(us.value(QLatin1String(k)).toDouble()), 18);
            }
            s += QStringLiteral("\n");
        }
        s += QStringLiteral(
            "\nNET is what Kite will let you deploy. It is NOT cash: it\n"
            "includes collateral and is reduced by everything under\n"
            "utilised. Sizing off `cash` alone overstates on a book with\n"
            "pledged holdings and understates one with open F&O.\n");
        funds_->setPlainText(s);
    }

    void render_positions(const QJsonObject& o) {
        if (!fetched(o, "positions")) {
            positions_->setPlainText(QStringLiteral(
                "positions: NOT FETCHED (HTTP %1)\n\nAbsent, not flat.")
                    .arg(o.value(QStringLiteral("positions_status")).toInt()));
            return;
        }
        const QJsonObject d =
            o.value(QStringLiteral("positions")).toObject()
             .value(QStringLiteral("data")).toObject();
        const QJsonArray net = d.value(QStringLiteral("net")).toArray();
        if (net.isEmpty()) {
            positions_->setPlainText(QStringLiteral(
                "No open positions.\n\nThis is EMPTY, which is different "
                "from not fetched — the call\nsucceeded and the book is "
                "genuinely flat."));
            return;
        }
        QString s = QStringLiteral("%1 %2 %3 %4 %5 %6\n")
                        .arg(QStringLiteral("symbol"), -22)
                        .arg(QStringLiteral("qty"), 8)
                        .arg(QStringLiteral("avg"), 12)
                        .arg(QStringLiteral("last"), 12)
                        .arg(QStringLiteral("P&L"), 14)
                        .arg(QStringLiteral("product"), 8);
        double total = 0.0;
        for (const auto& v : net) {
            const QJsonObject p = v.toObject();
            const double pnl = p.value(QStringLiteral("pnl")).toDouble();
            total += pnl;
            s += QStringLiteral("%1 %2 %3 %4 %5 %6\n")
                     .arg(p.value(QStringLiteral("tradingsymbol")).toString(), -22)
                     .arg(p.value(QStringLiteral("quantity")).toInt(), 8)
                     .arg(p.value(QStringLiteral("average_price")).toDouble(),
                          12, 'f', 2)
                     .arg(p.value(QStringLiteral("last_price")).toDouble(),
                          12, 'f', 2)
                     .arg(rupees(pnl), 14)
                     .arg(p.value(QStringLiteral("product")).toString(), 8);
        }
        s += QStringLiteral("\n%1 %2\n").arg(QStringLiteral("TOTAL P&L"), -22)
                 .arg(rupees(total), 46);
        s += QStringLiteral(
            "\nP&L here is the BROKER's number, at the snapshot instant.\n"
            "The engine's own is in the Audit Trail, and P12-04 exists to\n"
            "reconcile the two — if they disagree, one of them is wrong and\n"
            "neither knows which.\n");
        positions_->setPlainText(s);
    }

    void render_holdings(const QJsonObject& o) {
        if (!fetched(o, "holdings")) {
            holdings_->setPlainText(QStringLiteral(
                "holdings: NOT FETCHED (HTTP %1)")
                    .arg(o.value(QStringLiteral("holdings_status")).toInt()));
            return;
        }
        const QJsonArray h =
            o.value(QStringLiteral("holdings")).toObject()
             .value(QStringLiteral("data")).toArray();
        if (h.isEmpty()) {
            holdings_->setPlainText(QStringLiteral("No holdings."));
            return;
        }
        QString s = QStringLiteral("%1 %2 %3 %4 %5\n")
                        .arg(QStringLiteral("symbol"), -22)
                        .arg(QStringLiteral("qty"), 8)
                        .arg(QStringLiteral("avg cost"), 12)
                        .arg(QStringLiteral("last"), 12)
                        .arg(QStringLiteral("P&L"), 14);
        for (const auto& v : h) {
            const QJsonObject p = v.toObject();
            s += QStringLiteral("%1 %2 %3 %4 %5\n")
                     .arg(p.value(QStringLiteral("tradingsymbol")).toString(), -22)
                     .arg(p.value(QStringLiteral("quantity")).toInt(), 8)
                     .arg(p.value(QStringLiteral("average_price")).toDouble(),
                          12, 'f', 2)
                     .arg(p.value(QStringLiteral("last_price")).toDouble(),
                          12, 'f', 2)
                     .arg(rupees(p.value(QStringLiteral("pnl")).toDouble()), 14);
        }
        holdings_->setPlainText(s);
    }

    void render_orders(const QJsonObject& o) {
        if (!fetched(o, "orders")) {
            orders_->setPlainText(QStringLiteral(
                "orders: NOT FETCHED (HTTP %1)")
                    .arg(o.value(QStringLiteral("orders_status")).toInt()));
            return;
        }
        const QJsonArray a =
            o.value(QStringLiteral("orders")).toObject()
             .value(QStringLiteral("data")).toArray();
        if (a.isEmpty()) {
            orders_->setPlainText(QStringLiteral(
                "No orders today.\n\nEmpty, and the call succeeded."));
            return;
        }
        QString s = QStringLiteral("%1 %2 %3 %4 %5 %6\n")
                        .arg(QStringLiteral("time"), -10)
                        .arg(QStringLiteral("symbol"), -20)
                        .arg(QStringLiteral("side"), -5)
                        .arg(QStringLiteral("qty/filled"), 12)
                        .arg(QStringLiteral("price"), 11)
                        .arg(QStringLiteral("status"), -12);
        for (const auto& v : a) {
            const QJsonObject p = v.toObject();
            const QString ts =
                p.value(QStringLiteral("order_timestamp")).toString();
            s += QStringLiteral("%1 %2 %3 %4 %5 %6\n")
                     .arg(ts.right(8), -10)
                     .arg(p.value(QStringLiteral("tradingsymbol")).toString(), -20)
                     .arg(p.value(QStringLiteral("transaction_type")).toString(), -5)
                     .arg(QStringLiteral("%1/%2")
                              .arg(p.value(QStringLiteral("quantity")).toInt())
                              .arg(p.value(QStringLiteral("filled_quantity")).toInt()), 12)
                     .arg(p.value(QStringLiteral("average_price")).toDouble(),
                          11, 'f', 2)
                     .arg(p.value(QStringLiteral("status")).toString(), -12);
        }
        s += QStringLiteral(
            "\nRead-only. There is no cancel button and there will not be\n"
            "one: cancelling is a MUTATION, and oms/ is the only directory\n"
            "permitted to send one. See the API surface tab.\n");
        orders_->setPlainText(s);
    }

    [[nodiscard]] static QString catalogue_text() {
        QString s = QStringLiteral(
            "EVERY KITE ENDPOINT, from Zerodha's own Go client\n"
            "(research/reference/gokiteconnect/connect.go)\n\n"
            "Transcribed from the SOURCE, not the web docs — those two have\n"
            "disagreed before, and P0's reference note records two time\n"
            "formats with different semantics found exactly that way.\n\n"
            "  R = read-only     M = MUTATES something at the broker\n\n");
        QString group;
        int reads = 0, mutates = 0;
        for (const KiteEndpoint& e : kite_endpoints()) {
            if (group != QLatin1String(e.group)) {
                group = QLatin1String(e.group);
                s += QStringLiteral("\n%1\n").arg(group.toUpper());
            }
            s += QStringLiteral("  %1  %2\n      %3\n")
                     .arg(e.mutates ? QStringLiteral("M") : QStringLiteral("R"))
                     .arg(QLatin1String(e.path), -44)
                     .arg(QLatin1String(e.state));
            (e.mutates ? mutates : reads)++;
        }
        s += QStringLiteral(
            "\n\n%1 endpoints: %2 read, %3 mutating.\n\n"
            "NONE of the %3 mutating calls has a button in this window, and\n"
            "that is not a gap to be filled later. CLAUDE.md is unconditional:\n"
            "oms/ is the only thing that can place an order. The in-process\n"
            "decision gave up blast radius — a bad cast in a chart now takes\n"
            "down the process holding live positions — and kept exactly one\n"
            "property by construction in exchange: THE UI CANNOT TRADE.\n\n"
            "A greyed-out Place Order button would be worse than none. It\n"
            "would imply the wiring exists and is merely disabled.\n")
                 .arg(reads + mutates).arg(reads).arg(mutates);
        return s;
    }

    QString snapshot_, session_;
    QLabel* head_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QPlainTextEdit* funds_ = nullptr;
    QPlainTextEdit* positions_ = nullptr;
    QPlainTextEdit* holdings_ = nullptr;
    QPlainTextEdit* orders_ = nullptr;
    QPlainTextEdit* catalogue_ = nullptr;
};

} // namespace altair::ui
