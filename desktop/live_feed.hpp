// desktop/live_feed.hpp -- the live tick feed, in the window.
//
// P32-07. `app/kite_ticker_main.cpp` opens the WebSocket, decodes with
// `feed/kite_decoder.hpp` and writes a status file. This runs it and reads
// that file.
//
// SAME BOUNDARY AS EVERY OTHER LIVE THING IN THIS UI.
//
// `desktop/` may not link `broker/` -- the gate-3 allow-list refuses it at
// configure time -- so this launches a subprocess and reads what it wrote,
// exactly as Link Kite (P26-01), the quote refresh (P29-02) and the account
// fetch (P32-02) do. No credential enters this address space.
//
// IT IS A BOUNDED RUN, AND THE PANEL SAYS SO RATHER THAN IMPLYING A STREAM.
//
// The ticker listens for N seconds and exits. That is deliberate for now: a
// permanently-open socket owned by a subprocess needs supervision, restart
// policy and a story about what happens when the daily token expires
// mid-session, and none of that exists yet. A panel that showed a "LIVE" pill
// over a bounded sample would be claiming something the machinery does not do.
//
// So the button says how long it will listen, and the readout says when the
// sample was taken and how old it is. Age before value, the same rule the
// account and quote panels keep.
//
// CONNECTED AND SILENT IS ITS OWN STATE.
//
// A subscription outside market hours opens, authenticates, and delivers
// nothing but heartbeats. In any counter that lumps heartbeats in with data
// frames that is indistinguishable from a healthy feed. They are counted
// separately here and the panel names the state, because "the feed is broken"
// and "the market is shut" need different actions.

#pragma once

#include "account_widgets.hpp"
#include "auth.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace altair::ui {

class LiveFeedPanel final : public QWidget {
    Q_OBJECT

public:
    explicit LiveFeedPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 12, 12, 12);

        auto* head = new QLabel(
            QStringLiteral(
                "<b>LIVE TICKS — the WebSocket feed.</b><br>"
                "Runs <code>altair_kite_ticker</code> as a subprocess: it "
                "opens Kite's ticker, decodes with the same "
                "<code>feed/kite_decoder.hpp</code> the replayer uses, and "
                "writes a status file this panel reads. This window has no "
                "network and no credential.<br><br>"
                "<b>It is a bounded sample, not a stream.</b> The ticker "
                "listens for the seconds you choose and exits."),
            this);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral(
            "background:#161C22;color:#9FB3C8;padding:8px;"
            "border:1px solid #2C3E50;"));
        v->addWidget(head);

        auto* row = new QHBoxLayout;
        secs_ = new QSpinBox(this);
        secs_->setRange(5, 300);
        secs_->setValue(15);
        secs_->setSuffix(QStringLiteral(" s"));
        listen_ = new QPushButton(QStringLiteral("Listen"), this);
        listen_->setStyleSheet(action_button_css(true));
        status_ = new QLabel(this);
        status_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        row->addWidget(new QLabel(QStringLiteral("Listen for"), this));
        row->addWidget(secs_);
        row->addWidget(listen_);
        row->addWidget(status_, 1);
        v->addLayout(row);

        cards_ = new CardGrid(3, this);
        v->addWidget(cards_);

        last_ = account_table({QStringLiteral("Token"),
                               QStringLiteral("Last price")});
        v->addWidget(last_, 1);

        note_ = new QLabel(this);
        note_->setWordWrap(true);
        note_->setStyleSheet(QStringLiteral("color:#9FB3C8;font-size:11px;"));
        v->addWidget(note_);

        connect(listen_, &QPushButton::clicked, this, &LiveFeedPanel::listen);
        reload();
    }

public Q_SLOTS:
    void listen() {
        QString exe =
#if defined(_WIN32)
            QStringLiteral("altair_kite_ticker.exe");
#else
            QStringLiteral("altair_kite_ticker");
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
            status_->setText(QStringLiteral(
                "altair_kite_ticker is not in this build. It needs the `net` "
                "preset: build.bat net"));
            return;
        }

        listen_->setEnabled(false);
        status_->setText(QStringLiteral("listening for %1 s...")
                             .arg(secs_->value()));
        QApplication::processEvents();

        QProcess proc;
        proc.setProgram(found);
        proc.setArguments({QStringLiteral("--seconds"),
                           QString::number(secs_->value()),
                           QStringLiteral("--go")});
        proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef ALTAIR_SOURCE_DIR
        // Pinned for the reason P26-02b found the hard way: a subprocess
        // launched from a desktop shortcut writes its relative output path
        // into the build folder, where nothing reads it, and reports success.
        proc.setWorkingDirectory(QStringLiteral(ALTAIR_SOURCE_DIR));
#endif
        proc.start();
        const bool ok =
            proc.waitForStarted(5000)
            && proc.waitForFinished((secs_->value() + 30) * 1000);
        listen_->setEnabled(true);
        if (!ok) {
            proc.kill();
            status_->setText(QStringLiteral("the ticker did not finish"));
            return;
        }
        if (proc.exitCode() != 0) {
            status_->setText(
                QStringLiteral("feed failed: %1")
                    .arg(QString::fromUtf8(proc.readAll()).trimmed()
                             .section(QChar('\n'), -2)));
            return;
        }
        status_->clear();
        reload();
    }

    void reload() {
        cards_->clear();
        last_->setRowCount(0);

        QFile f(QStringLiteral(ALTAIR_TICKS_FILE));
        if (!f.open(QIODevice::ReadOnly)) {
            cards_->add_wide(new StatCard(
                QStringLiteral("feed"), QStringLiteral("never run"), 0,
                QStringLiteral("No sample has been taken in this tree.")));
            note_->setText(QString());
            return;
        }
        const auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject()) {
            cards_->add_wide(new StatCard(
                QStringLiteral("feed"), QStringLiteral("unreadable"), -1));
            return;
        }
        const QJsonObject o = doc.object();

        // AGE BEFORE VALUE. A tick count with no timestamp gets believed.
        const qint64 at =
            static_cast<qint64>(o.value(QStringLiteral("fetched_at_unix"))
                                    .toDouble());
        const qint64 age = QDateTime::currentSecsSinceEpoch() - at;
        const QString age_s =
            age < 90    ? QStringLiteral("%1 s ago").arg(age)
            : age < 5400 ? QStringLiteral("%1 min ago").arg(age / 60)
            : age < 172800 ? QStringLiteral("%1 h ago").arg(age / 3600)
                           : QStringLiteral("%1 DAYS ago").arg(age / 86400);

        const int frames = o.value(QStringLiteral("binary_frames")).toInt();
        const int beats = o.value(QStringLiteral("heartbeats")).toInt();
        const int ticks = o.value(QStringLiteral("ticks")).toInt();
        const int depth = o.value(QStringLiteral("depth_updates")).toInt();
        const int bad = o.value(QStringLiteral("undecodable")).toInt();
        const int unk = o.value(QStringLiteral("unknown_token")).toInt();

        cards_->add_wide(new StatCard(
            QStringLiteral("sample taken"), age_s, 2,
            QStringLiteral("%1 s of listening, mode %2")
                .arg(o.value(QStringLiteral("seconds")).toInt())
                .arg(o.value(QStringLiteral("mode")).toString())));
        cards_->add(new StatCard(QStringLiteral("ticks decoded"),
                                 QString::number(ticks),
                                 ticks > 0 ? 1 : 0));
        cards_->add(new StatCard(QStringLiteral("data frames"),
                                 QString::number(frames)));
        cards_->add(new StatCard(QStringLiteral("heartbeats"),
                                 QString::number(beats)));
        cards_->add(new StatCard(QStringLiteral("depth updates"),
                                 QString::number(depth)));
        cards_->add(new StatCard(QStringLiteral("undecodable"),
                                 QString::number(bad), bad > 0 ? -1 : 0));
        cards_->add(new StatCard(QStringLiteral("unknown token"),
                                 QString::number(unk), unk > 0 ? -1 : 0));

        const QJsonArray lasts = o.value(QStringLiteral("last")).toArray();

        // SORTING OFF WHILE THE ROWS GO IN, AND THAT IS NOT A TIDINESS RULE.
        //
        // account_table() enables sorting, and a QTableWidget that is sorting
        // RE-SORTS ON EVERY setItem. Insert a row, set its token, and the
        // table may move that row before the price cell is written -- so the
        // price lands on whichever row is now at that index.
        //
        // Caught on screen: India VIX's 11.57 appeared on the NIFTY 50 row
        // while the other two prices were blank. Nothing about that looks
        // like a bug from a distance; it looks like NIFTY trading at 11.57
        // and two instruments not quoting, which is a statement about the
        // market rather than about the table.
        //
        // The account panel's three renderers already do this. This one did
        // not, which is why it is worth a comment rather than a one-liner.
        last_->setSortingEnabled(false);
        int r = 0;
        for (const auto& v : lasts) {
            const QJsonObject e = v.toObject();
            last_->insertRow(r);
            account_cell(last_, r, 0,
                         QString::number(
                             e.value(QStringLiteral("token")).toInteger()));
            account_cell(
                last_, r, 1,
                QString::number(
                    static_cast<double>(
                        e.value(QStringLiteral("last_price_paise")).toInteger())
                        / 100.0,
                    'f', 2),
                true);
            ++r;
        }
        last_->setSortingEnabled(true);

        // THE THREE STATES, NAMED. "The feed is broken", "the market is shut"
        // and "this is working" need different actions and look identical in
        // a single frame count.
        if (frames == 0 && beats == 0) {
            note_->setText(QStringLiteral(
                "<b>NOTHING ARRIVED AT ALL</b> — not even a heartbeat. That "
                "is a connection problem, not a quiet market."));
        } else if (frames == 0) {
            note_->setText(QStringLiteral(
                "<b>CONNECTED, AND NOTHING TICKED.</b> The socket opened, the "
                "subscription was accepted, and only heartbeats arrived. That "
                "is what a closed market looks like — it is not a failure and "
                "it is not a healthy feed either."));
        } else if (depth == 0) {
            note_->setText(QStringLiteral(
                "<b>%1 ticks, and no depth.</b> That is correct for the "
                "default subscription: NIFTY 50, NIFTY BANK and INDIA VIX are "
                "INDICES, and an index has no order book to publish. Kite "
                "sends them as 32-byte index packets with no depth section, "
                "in <i>full</i> mode or any other. Depth needs a tradeable "
                "instrument — a future or an option.").arg(ticks));
        } else {
            note_->setText(QStringLiteral(
                "<b>%1 ticks and %2 depth updates</b>, %3 undecodable. Every "
                "one went through the same decoder the replayer uses — rule 6, "
                "and the reason a backtest of this data is not a lie.")
                    .arg(ticks).arg(depth).arg(bad));
        }
    }

private:
    QSpinBox* secs_ = nullptr;
    QPushButton* listen_ = nullptr;
    QLabel* status_ = nullptr;
    CardGrid* cards_ = nullptr;
    QTableWidget* last_ = nullptr;
    QLabel* note_ = nullptr;
};

} // namespace altair::ui
