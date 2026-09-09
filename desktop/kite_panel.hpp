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

#include "account_widgets.hpp"
#include "feed_status.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QProcess>
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

        // ---- THE ACTION BAR ------------------------------------------
        //
        // P32-02. "Refresh" used to PRINT THE COMMAND TO RUN. That was honest
        // about the boundary and useless at it: the operator read a command
        // off a window, switched to a shell, and typed it. The boundary is
        // that desktop/ may not LINK broker/ -- not that it may not start a
        // process -- and P26-01 and P29-02 had already established the shape,
        // which is to launch the read-only fetcher and re-read what it wrote.
        //
        // The credential still never enters this address space. The subprocess
        // reads data/kite_session.json; this window does not, and cannot.
        auto* bar = new QWidget;
        auto* bl = new QHBoxLayout(bar);
        bl->setContentsMargins(14, 4, 14, 8);
        bl->setSpacing(8);

        fetch_ = new QPushButton(QStringLiteral("Fetch from Kite"));
        fetch_->setStyleSheet(action_button_css(true));
        fetch_->setToolTip(QStringLiteral(
            "Runs altair_kite_account --go as a subprocess and re-reads the "
            "snapshot it writes. Read-only: no endpoint it calls can place, "
            "modify or cancel anything."));

        reread_ = new QPushButton(QStringLiteral("Re-read file"));
        reread_->setStyleSheet(action_button_css(false));
        reread_->setToolTip(QStringLiteral(
            "Re-reads the snapshot already on disk. Makes no network call."));

        status_ = new QLabel;
        status_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));

        bl->addWidget(fetch_);
        bl->addWidget(reread_);
        bl->addWidget(status_, 1);
        v->addWidget(bar);

        connect(reread_, &QPushButton::clicked, this, [this] { reload(); });
        connect(fetch_, &QPushButton::clicked, this, [this] { fetch(); });

        tabs_ = new QTabWidget;

        // Funds is CARDS. It is the tab that gets opened to read one number.
        funds_page_ = new QWidget;
        auto* fv = new QVBoxLayout(funds_page_);
        fv->setContentsMargins(14, 14, 14, 14);
        funds_cards_ = new CardGrid(4, funds_page_);
        fv->addWidget(funds_cards_);
        funds_note_ = new QLabel(funds_page_);
        funds_note_->setWordWrap(true);
        funds_note_->setStyleSheet(QStringLiteral(
            "color:#7F8C8D;font-size:11px;"));
        fv->addWidget(funds_note_);
        fv->addStretch(1);

        positions_t_ = account_table({QStringLiteral("Symbol"),
                                      QStringLiteral("Qty"),
                                      QStringLiteral("Avg"),
                                      QStringLiteral("Last"),
                                      QStringLiteral("P&L"),
                                      QStringLiteral("Product")});
        holdings_t_ = account_table({QStringLiteral("Symbol"),
                                     QStringLiteral("Qty"),
                                     QStringLiteral("Avg cost"),
                                     QStringLiteral("Last"),
                                     QStringLiteral("P&L")});
        orders_t_ = account_table({QStringLiteral("Time"),
                                   QStringLiteral("Symbol"),
                                   QStringLiteral("Side"),
                                   QStringLiteral("Qty"),
                                   QStringLiteral("Price"),
                                   QStringLiteral("Status")});

        positions_page_ = wrap_table(positions_t_, &positions_note_);
        holdings_page_ = wrap_table(holdings_t_, &holdings_note_);
        orders_page_ = wrap_table(orders_t_, &orders_note_);

        catalogue_ = make_pane();
        tabs_->addTab(funds_page_, QStringLiteral("Funds"));
        tabs_->addTab(positions_page_, QStringLiteral("Positions"));
        tabs_->addTab(holdings_page_, QStringLiteral("Holdings"));
        tabs_->addTab(orders_page_, QStringLiteral("Orders"));
        tabs_->addTab(catalogue_, QStringLiteral("API surface"));
        v->addWidget(tabs_, 1);

        catalogue_->setPlainText(catalogue_text());
        reload();
    }

private:
    /// A table plus the note under it that says what an empty one MEANS.
    ///
    /// The note is not decoration. An empty positions table and an unfetched
    /// positions table look identical, and the difference is the difference
    /// between "I am flat" and "I do not know". Every list on this page
    /// carries one.
    [[nodiscard]] QWidget* wrap_table(QTableWidget* t, QLabel** note_out) {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        auto* note = new QLabel(page);
        note->setWordWrap(true);
        note->setContentsMargins(14, 10, 14, 10);
        note->setStyleSheet(QStringLiteral(
            "color:#9FB3C8;background:#161C22;font-size:11px;"
            "border-bottom:1px solid #2C3E50;"));
        v->addWidget(note);
        v->addWidget(t, 1);
        *note_out = note;
        return page;
    }

    /// Run the READ-ONLY account fetcher, then re-read what it wrote.
    ///
    /// Same shape as the watchlist's quote refresh and Link Kite's login: a
    /// subprocess, with the working directory pinned. P26-02b is why the pin
    /// is here and not assumed -- a subprocess launched from a desktop
    /// shortcut writes its relative output path into the build folder, where
    /// nothing reads it, while reporting success.
    void fetch() {
        QString exe =
#if defined(_WIN32)
            QStringLiteral("altair_kite_account.exe");
#else
            QStringLiteral("altair_kite_account");
#endif
        QStringList tried;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../app/") + exe;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
        tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
        QString found;
        for (const QString& c : tried) {
            if (QFileInfo(c).isFile()) {
                found = QFileInfo(c).canonicalFilePath();
                break;
            }
        }
        if (found.isEmpty()) {
            // NAMED, not "failed". The fetcher only exists in a preset with
            // networking, and "not built here" is a different problem from
            // "the token expired" -- they need different actions.
            status_->setText(QStringLiteral(
                "altair_kite_account is not in this build. It needs the "
                "`net` preset: build.bat net"));
            return;
        }

        fetch_->setEnabled(false);
        status_->setText(QStringLiteral("fetching..."));
        QApplication::processEvents();

        QProcess proc;
        proc.setProgram(found);
        proc.setArguments({QStringLiteral("--go")});
        proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef ALTAIR_SOURCE_DIR
        proc.setWorkingDirectory(QStringLiteral(ALTAIR_SOURCE_DIR));
#endif
        proc.start();
        const bool ok = proc.waitForStarted(5000)
                        && proc.waitForFinished(45000);
        fetch_->setEnabled(true);
        if (!ok) {
            proc.kill();
            status_->setText(QStringLiteral("the fetcher did not finish"));
            return;
        }
        if (proc.exitCode() != 0) {
            // Kite's own words. A token that expired daily and a network that
            // is down are different problems and read differently.
            status_->setText(
                QStringLiteral("fetch failed: %1")
                    .arg(QString::fromUtf8(proc.readAll()).trimmed()
                             .section(QChar('\n'), -2)));
            return;
        }
        status_->setText(QString());
        reload();
    }

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
            funds_cards_->clear();
            funds_cards_->add_wide(new StatCard(
                QStringLiteral("funds"), QStringLiteral("not fetched"), 0,
                QStringLiteral("ABSENT, not zero. Press Fetch from Kite.")));
            funds_note_->setText(QString());
            for (auto* t : {positions_t_, holdings_t_, orders_t_}) {
                t->setRowCount(0);
            }
            for (auto* nt : {positions_note_, holdings_note_, orders_note_}) {
                nt->setText(QStringLiteral(
                    "<b>NOT FETCHED.</b> This is ABSENT, not empty — it does "
                    "not mean you hold nothing."));
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
        funds_cards_->clear();
        if (!fetched(o, "margins")) {
            funds_cards_->add_wide(new StatCard(
                QStringLiteral("margins"), QStringLiteral("NOT FETCHED"), 0,
                QStringLiteral("HTTP %1. Absent, not zero.")
                    .arg(o.value(QStringLiteral("margins_status")).toInt())));
            funds_note_->setText(QString());
            return;
        }
        const QJsonObject d =
            o.value(QStringLiteral("margins")).toObject()
             .value(QStringLiteral("data")).toObject();

        for (const QString& seg : {QStringLiteral("equity"),
                                   QStringLiteral("commodity")}) {
            if (!d.contains(seg)) { continue; }
            const QJsonObject m = d.value(seg).toObject();
            const QJsonObject av =
                m.value(QStringLiteral("available")).toObject();
            const QJsonObject us =
                m.value(QStringLiteral("utilised")).toObject();
            const bool on = m.value(QStringLiteral("enabled")).toBool();
            const double net = m.value(QStringLiteral("net")).toDouble();

            // NET IS THE HEADLINE AND IS NOT CASH. It is the one number a
            // sizing decision starts from, so it gets the wide card and the
            // sentence -- the text version buried that sentence at the bottom
            // of a wall of aligned digits.
            funds_cards_->add_wide(new StatCard(
                QStringLiteral("%1 — net available").arg(seg),
                rupees(net), 2,
                on ? QStringLiteral(
                         "What Kite will let you deploy. NOT cash: it includes "
                         "collateral and is reduced by everything utilised.")
                   : QStringLiteral("SEGMENT NOT ENABLED on this account.")));

            for (const auto& [k, label] : {
                     std::pair{"cash", "cash"},
                     {"opening_balance", "opening balance"},
                     {"live_balance", "live balance"},
                     {"collateral", "collateral"},
                     {"intraday_payin", "intraday payin"},
                     {"adhoc_margin", "adhoc margin"}}) {
                funds_cards_->add(new StatCard(
                    QLatin1String(label),
                    rupees(av.value(QLatin1String(k)).toDouble())));
            }
            for (const auto& [k, label] : {
                     std::pair{"debits", "utilised — debits"},
                     {"span", "utilised — SPAN"},
                     {"exposure", "utilised — exposure"},
                     {"option_premium", "utilised — option premium"},
                     {"delivery", "utilised — delivery"},
                     {"payout", "utilised — payout"},
                     {"turnover", "utilised — turnover"}}) {
                funds_cards_->add(new StatCard(
                    QLatin1String(label),
                    rupees(us.value(QLatin1String(k)).toDouble())));
            }
            // M2M is SIGNED and is the one pair here where colour means
            // something. The rest are magnitudes.
            for (const auto& [k, label] : {
                     std::pair{"m2m_realised", "M2M realised"},
                     {"m2m_unrealised", "M2M unrealised"}}) {
                const double x = us.value(QLatin1String(k)).toDouble();
                funds_cards_->add(new StatCard(
                    QLatin1String(label), rupees(x),
                    x > 0.0 ? 1 : (x < 0.0 ? -1 : 0)));
            }
        }
        funds_note_->setText(QStringLiteral(
            "Sizing off <b>cash</b> alone overstates on a book with pledged "
            "holdings and understates one with open F&amp;O. The number to "
            "size from is <b>net</b>."));
    }

    void render_positions(const QJsonObject& o) {
        positions_t_->setRowCount(0);
        if (!fetched(o, "positions")) {
            positions_note_->setText(QStringLiteral(
                "<b>NOT FETCHED</b> (HTTP %1). Absent, not flat.")
                    .arg(o.value(QStringLiteral("positions_status")).toInt()));
            return;
        }
        const QJsonArray net =
            o.value(QStringLiteral("positions")).toObject()
             .value(QStringLiteral("data")).toObject()
             .value(QStringLiteral("net")).toArray();
        if (net.isEmpty()) {
            positions_note_->setText(QStringLiteral(
                "<b>No open positions.</b> This is EMPTY, which is different "
                "from not fetched — the call succeeded and the book is "
                "genuinely flat."));
            return;
        }
        positions_t_->setSortingEnabled(false);
        double total = 0.0;
        int r = 0;
        for (const auto& v : net) {
            const QJsonObject q = v.toObject();
            const double pnl = q.value(QStringLiteral("pnl")).toDouble();
            total += pnl;
            positions_t_->insertRow(r);
            account_cell(positions_t_, r, 0,
                         q.value(QStringLiteral("tradingsymbol")).toString());
            account_cell(positions_t_, r, 1,
                         QString::number(
                             q.value(QStringLiteral("quantity")).toInt()),
                         true);
            account_cell(positions_t_, r, 2,
                         QString::number(
                             q.value(QStringLiteral("average_price")).toDouble(),
                             'f', 2), true);
            account_cell(positions_t_, r, 3,
                         QString::number(
                             q.value(QStringLiteral("last_price")).toDouble(),
                             'f', 2), true);
            account_cell(positions_t_, r, 4, rupees(pnl), true, pnl, true);
            account_cell(positions_t_, r, 5,
                         q.value(QStringLiteral("product")).toString());
            ++r;
        }
        positions_t_->setSortingEnabled(true);
        positions_note_->setText(QStringLiteral(
            "<b>Total P&amp;L %1</b> across %2 position(s). This is the "
            "BROKER's number at the snapshot instant; the engine's own is in "
            "the Audit Trail, and P12-04 exists to reconcile the two — if they "
            "disagree, one is wrong and neither knows which.")
                .arg(rupees(total)).arg(net.size()));
    }

    void render_holdings(const QJsonObject& o) {
        holdings_t_->setRowCount(0);
        if (!fetched(o, "holdings")) {
            holdings_note_->setText(QStringLiteral(
                "<b>NOT FETCHED</b> (HTTP %1). Absent, not empty.")
                    .arg(o.value(QStringLiteral("holdings_status")).toInt()));
            return;
        }
        const QJsonArray h =
            o.value(QStringLiteral("holdings")).toObject()
             .value(QStringLiteral("data")).toArray();
        if (h.isEmpty()) {
            holdings_note_->setText(QStringLiteral(
                "<b>No holdings.</b> Empty, and the call succeeded."));
            return;
        }
        holdings_t_->setSortingEnabled(false);
        double total = 0.0;
        int r = 0;
        for (const auto& v : h) {
            const QJsonObject q = v.toObject();
            const double pnl = q.value(QStringLiteral("pnl")).toDouble();
            total += pnl;
            holdings_t_->insertRow(r);
            account_cell(holdings_t_, r, 0,
                         q.value(QStringLiteral("tradingsymbol")).toString());
            account_cell(holdings_t_, r, 1,
                         QString::number(
                             q.value(QStringLiteral("quantity")).toInt()),
                         true);
            account_cell(holdings_t_, r, 2,
                         QString::number(
                             q.value(QStringLiteral("average_price")).toDouble(),
                             'f', 2), true);
            account_cell(holdings_t_, r, 3,
                         QString::number(
                             q.value(QStringLiteral("last_price")).toDouble(),
                             'f', 2), true);
            account_cell(holdings_t_, r, 4, rupees(pnl), true, pnl, true);
            ++r;
        }
        holdings_t_->setSortingEnabled(true);
        holdings_note_->setText(QStringLiteral(
            "<b>Total P&amp;L %1</b> across %2 holding(s).")
                .arg(rupees(total)).arg(h.size()));
    }

    void render_orders(const QJsonObject& o) {
        orders_t_->setRowCount(0);
        if (!fetched(o, "orders")) {
            orders_note_->setText(QStringLiteral(
                "<b>NOT FETCHED</b> (HTTP %1). Absent, not none.")
                    .arg(o.value(QStringLiteral("orders_status")).toInt()));
            return;
        }
        const QJsonArray a =
            o.value(QStringLiteral("orders")).toObject()
             .value(QStringLiteral("data")).toArray();
        if (a.isEmpty()) {
            orders_note_->setText(QStringLiteral(
                "<b>No orders today.</b> Empty, and the call succeeded. "
                "Read-only: there is no cancel button and there will not be "
                "one — cancelling is a MUTATION and <code>oms/</code> is the "
                "only directory permitted to send one."));
            return;
        }
        orders_t_->setSortingEnabled(false);
        int r = 0;
        int open = 0;
        for (const auto& v : a) {
            const QJsonObject q = v.toObject();
            const QString st = q.value(QStringLiteral("status")).toString();
            const int qty = q.value(QStringLiteral("quantity")).toInt();
            const int fil =
                q.value(QStringLiteral("filled_quantity")).toInt();
            const QString side =
                q.value(QStringLiteral("transaction_type")).toString();
            if (st != QStringLiteral("COMPLETE")
                && st != QStringLiteral("CANCELLED")
                && st != QStringLiteral("REJECTED")) {
                ++open;
            }
            orders_t_->insertRow(r);
            account_cell(orders_t_, r, 0,
                         q.value(QStringLiteral("order_timestamp"))
                             .toString().right(8));
            account_cell(orders_t_, r, 1,
                         q.value(QStringLiteral("tradingsymbol")).toString());
            // SIDE is tinted; BUY green, SELL red. That is a direction, not a
            // profit, and it is the one place on this page where the two
            // colours mean something other than P&L -- which is exactly why
            // the quantity column beside it is NOT tinted: a negative
            // quantity is a short, not a loss.
            account_cell(orders_t_, r, 2, side, false,
                         side == QStringLiteral("BUY") ? 1.0 : -1.0, true);
            account_cell(orders_t_, r, 3,
                         QStringLiteral("%1 / %2").arg(fil).arg(qty), true);
            account_cell(orders_t_, r, 4,
                         QString::number(
                             q.value(QStringLiteral("average_price")).toDouble(),
                             'f', 2), true);
            account_cell(orders_t_, r, 5, st, false,
                         st == QStringLiteral("COMPLETE")   ? 1.0
                         : st == QStringLiteral("REJECTED") ? -1.0
                                                            : 0.0,
                         true);
            ++r;
        }
        orders_t_->setSortingEnabled(true);
        orders_note_->setText(QStringLiteral(
            "%1 order(s) today, <b>%2 still working</b>. Read-only: there is "
            "no cancel button and there will not be one — cancelling is a "
            "MUTATION and <code>oms/</code> is the only directory permitted "
            "to send one. See the API surface tab.")
                .arg(a.size()).arg(open));
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
    QPushButton* fetch_ = nullptr;
    QPushButton* reread_ = nullptr;
    QLabel* status_ = nullptr;

    QWidget* funds_page_ = nullptr;
    CardGrid* funds_cards_ = nullptr;
    QLabel* funds_note_ = nullptr;

    QWidget* positions_page_ = nullptr;
    QTableWidget* positions_t_ = nullptr;
    QLabel* positions_note_ = nullptr;

    QWidget* holdings_page_ = nullptr;
    QTableWidget* holdings_t_ = nullptr;
    QLabel* holdings_note_ = nullptr;

    QWidget* orders_page_ = nullptr;
    QTableWidget* orders_t_ = nullptr;
    QLabel* orders_note_ = nullptr;

    /// The endpoint catalogue stays TEXT. It is a reference document read
    /// once end to end, not a page scanned for one number, and a table of
    /// 49 rows with a prose "where this has to live" column would be less
    /// readable than the aligned list it already is.
    QPlainTextEdit* catalogue_ = nullptr;
};

} // namespace altair::ui
