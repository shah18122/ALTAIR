// desktop/server_page.hpp -- the Linux server ALTAIR runs on, from inside ALTAIR.
//
// ALTAIR runs on one Linux box (in production an EC2 server whose screen is
// Amazon DCV). This page shows that box:
//   SCHEDULED JOBS   each systemd timer ops/install.sh registers: when it last
//                    ran, how that ended, and when it runs next; its log
//                    (journalctl) on a click.
//   LATENCY          the host's tuning as in effect now (core/host_tuning.hpp:
//                    isolated cores, hugepages, limits, busy polling) and the
//                    latest altair_latency_probe report; "Run probe" runs it.
//   INSTALL / UPDATE "Install or update ALTAIR" opens a terminal running
//                    sudo ops/setup.sh: packages, Qt, git pull, build, timers.
//
// Reads; it changes nothing on its own. systemctl and journalctl run without
// privileges (reading only); the update asks for the sudo password in its own
// terminal window.
#pragma once

#include "theme.hpp"

#include <core/host_tuning.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QShowEvent>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

#include <array>

namespace altair::ui {

/// One scheduled job: its timer and service, and when it runs (IST, Mon-Fri).
struct ServerJob {
    const char* unit;      ///< without .timer / .service
    const char* service;   ///< the service the timer starts
    const char* when;
    const char* what;
};

inline constexpr std::array<ServerJob, 6> kServerJobs{{
    {"altair-fyers-login", "altair-fyers-login", "08:30", "today's FYERS login (TOTP + PIN)"},
    {"altair-recorder", "altair-recorder", "08:50", "records the session to 15:45"},
    {"altair-chain-open", "altair-chain@open", "09:20", "every NIFTY/BANKNIFTY strike"},
    {"altair-chain-close", "altair-chain@close", "15:27", "every strike, before the close"},
    {"altair-fyers-chain", "altair-fyers-chain", "15:45", "the day's chains from FYERS"},
    {"altair-nightly", "altair-nightly", "20:00", "store, snapshot, retrain, push"},
}};

/// `systemctl show -p A -p B ...` output ("Key=value" lines) as a map.
[[nodiscard]] inline QMap<QString, QString> parse_systemctl_show(const QString& text) {
    QMap<QString, QString> m;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) m.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return m;
}

/// How a job last ended, in words, from its service's properties.
[[nodiscard]] inline QString job_outcome(const QMap<QString, QString>& svc) {
    const QString load = svc.value(QStringLiteral("LoadState"));
    if (load == QLatin1String("not-found") || load.isEmpty()) return QStringLiteral("not installed (sudo ops/install.sh --go)");
    const QString active = svc.value(QStringLiteral("ActiveState"));
    if (active == QLatin1String("active") || active == QLatin1String("activating")) return QStringLiteral("running now");
    const QString result = svc.value(QStringLiteral("Result"));
    const QString code = svc.value(QStringLiteral("ExecMainStatus"));
    if (svc.value(QStringLiteral("ExecMainStartTimestamp")).isEmpty()) return QStringLiteral("not run yet");
    if (result == QLatin1String("success")) return QStringLiteral("ok");
    return QStringLiteral("ended: %1%2").arg(result, code.isEmpty() || code == QLatin1String("0") ? QString() : QStringLiteral(" (exit %1)").arg(code));
}

/// The terminal command the Update button runs.
[[nodiscard]] inline QStringList update_command(const QString& root) {
    return {QStringLiteral("x-terminal-emulator"), QStringLiteral("-e"), QStringLiteral("bash"), QStringLiteral("-c"),
            QStringLiteral("sudo '%1/ops/setup.sh'; echo; read -rp 'Done. Press Enter to close. '").arg(root)};
}

/// The newest probe report under data/verified/latency, or "".
[[nodiscard]] inline QString latest_probe(const QString& root) {
    const QDir d(root + QStringLiteral("/data/verified/latency"));
    const QFileInfoList f = d.entryInfoList({QStringLiteral("probe-*.txt")}, QDir::Files, QDir::Name | QDir::Reversed);
    return f.isEmpty() ? QString() : f.first().absoluteFilePath();
}

class ServerPage final : public QWidget {
public:
    explicit ServerPage(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("serverPage"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(16, 14, 16, 12);
        v->setSpacing(10);
        auto* head = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("Server"), this);
        set_role(title, QStringLiteral("title"));
        head->addWidget(title);
        head->addStretch();
        auto* refresh = new QPushButton(QStringLiteral("Refresh"), this);
        head->addWidget(refresh);
        probe_btn_ = new QPushButton(QStringLiteral("Run latency probe"), this);
        probe_btn_->setToolTip(QStringLiteral("build/linux/app/altair_latency_probe: FYERS round trip, timer jitter, memory locking"));
        head->addWidget(probe_btn_);
        update_btn_ = new QPushButton(QStringLiteral("Install or update ALTAIR"), this);
        set_variant(update_btn_, QStringLiteral("primary"));
        update_btn_->setToolTip(QStringLiteral("Opens a terminal running sudo ops/setup.sh: packages, Qt, git pull, build, "
                                               "scheduled jobs. It refuses during market hours."));
        head->addWidget(update_btn_);
        v->addLayout(head);

        auto* note = new QLabel(QStringLiteral(
            "The Linux machine ALTAIR runs on: its scheduled jobs (systemd, IST times, Monday to Friday), how it is tuned "
            "for latency, and the one button that installs or updates everything. Nothing here trades."), this);
        note->setWordWrap(true);
        set_role(note, QStringLiteral("note"));
        v->addWidget(note);

        auto* split = new QSplitter(Qt::Vertical, this);
        jobs_ = new QTableWidget(static_cast<int>(kServerJobs.size()), 5, split);
        jobs_->setObjectName(QStringLiteral("serverJobs"));
        jobs_->setHorizontalHeaderLabels({QStringLiteral("Job"), QStringLiteral("At"), QStringLiteral("Last run"),
                                          QStringLiteral("Outcome"), QStringLiteral("Next run")});
        jobs_->verticalHeader()->hide();
        jobs_->horizontalHeader()->setStretchLastSection(true);
        jobs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        jobs_->setSelectionBehavior(QAbstractItemView::SelectRows);
        for (int r = 0; r < static_cast<int>(kServerJobs.size()); ++r) {
            const ServerJob& j = kServerJobs[static_cast<std::size_t>(r)];
            auto* name = new QTableWidgetItem(QString::fromLatin1(j.unit));
            name->setToolTip(QString::fromLatin1(j.what));
            jobs_->setItem(r, 0, name);
            jobs_->setItem(r, 1, new QTableWidgetItem(QString::fromLatin1(j.when)));
            for (int c = 2; c < 5; ++c) jobs_->setItem(r, c, new QTableWidgetItem(QStringLiteral("—")));
        }
        detail_ = new QTextBrowser(split);
        detail_->setObjectName(QStringLiteral("serverDetail"));
        split->addWidget(jobs_);
        split->addWidget(detail_);
        split->setStretchFactor(1, 2);
        v->addWidget(split, 1);
        status_ = new QLabel(this);
        set_role(status_, QStringLiteral("note"));
        v->addWidget(status_);

        QObject::connect(refresh, &QPushButton::clicked, this, [this] { refresh_all(); });
        QObject::connect(probe_btn_, &QPushButton::clicked, this, [this] { run_probe(); });
        QObject::connect(update_btn_, &QPushButton::clicked, this, [this] { start_update(); });
        QObject::connect(jobs_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { show_log(row); });
    }

    [[nodiscard]] QTableWidget* jobs() const { return jobs_; }
    [[nodiscard]] QTextBrowser* detail() const { return detail_; }
    [[nodiscard]] QPushButton* update_button() const { return update_btn_; }

    /// The latency section as HTML: the tuning in effect, then the latest probe.
    [[nodiscard]] QString latency_html() const {
        QString h = QStringLiteral("<h3>Latency tuning in effect</h3><table cellspacing='4'>");
        for (const auto& it : altair::host::read_host_tuning().items()) {
            h += QStringLiteral("<tr><td style='color:%1'>%2</td><td>%3</td><td>%4</td><td style='color:%5'>%6</td></tr>")
                     .arg(it.good ? QStringLiteral("#3FB950") : QStringLiteral("#D29922"),
                          it.good ? QStringLiteral("ok") : QStringLiteral("todo"),
                          QString::fromStdString(it.name).toHtmlEscaped(), QString::fromStdString(it.value).toHtmlEscaped(),
                          QString::fromLatin1(theme_token::kTextMuted),
                          it.good ? QString() : QString::fromStdString(it.advice).toHtmlEscaped());
        }
        h += QStringLiteral("</table><p style='color:%1'>sudo ops/tune_host.sh shows every change it would make; "
                            "--go applies them (reboot for the isolated cores).</p>").arg(QString::fromLatin1(theme_token::kTextMuted));
        const QString p = latest_probe(root_);
        if (p.isEmpty()) {
            h += QStringLiteral("<h3>Latency probe</h3><p>No report yet: press <b>Run latency probe</b>.</p>");
        } else {
            QFile f(p);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                h += QStringLiteral("<h3>Latest probe (%1)</h3><pre>%2</pre>")
                         .arg(QFileInfo(p).fileName().toHtmlEscaped(), QString::fromUtf8(f.readAll()).toHtmlEscaped());
        }
        return h;
    }

    void refresh_all() {
        for (int r = 0; r < static_cast<int>(kServerJobs.size()); ++r) refresh_job(r);
        detail_->setHtml(latency_html());
        status_->setText(QStringLiteral("Read at %1. Double-click a job for its log.")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        if (!shown_) { shown_ = true; refresh_all(); }
    }

private:
    [[nodiscard]] static QString run(const QString& program, const QStringList& args, int ms = 3000) {
        QProcess p;
        p.start(program, args);
        if (!p.waitForFinished(ms)) { p.kill(); p.waitForFinished(500); return {}; }
        return QString::fromUtf8(p.readAllStandardOutput());
    }

    void refresh_job(int row) {
        const ServerJob& j = kServerJobs[static_cast<std::size_t>(row)];
        const auto timer = parse_systemctl_show(run(QStringLiteral("systemctl"),
            {QStringLiteral("show"), QString::fromLatin1(j.unit) + QStringLiteral(".timer"),
             QStringLiteral("-p"), QStringLiteral("LastTriggerUSec"), QStringLiteral("-p"), QStringLiteral("NextElapseUSecRealtime")}));
        const auto svc = parse_systemctl_show(run(QStringLiteral("systemctl"),
            {QStringLiteral("show"), QString::fromLatin1(j.service) + QStringLiteral(".service"),
             QStringLiteral("-p"), QStringLiteral("LoadState"), QStringLiteral("-p"), QStringLiteral("ActiveState"),
             QStringLiteral("-p"), QStringLiteral("Result"), QStringLiteral("-p"), QStringLiteral("ExecMainStatus"),
             QStringLiteral("-p"), QStringLiteral("ExecMainStartTimestamp")}));
        const auto shown = [](const QString& s) { return s.isEmpty() || s == QLatin1String("n/a") ? QStringLiteral("—") : s; };
        jobs_->item(row, 2)->setText(shown(timer.value(QStringLiteral("LastTriggerUSec"))));
        jobs_->item(row, 3)->setText(svc.isEmpty() ? QStringLiteral("systemctl not available") : job_outcome(svc));
        jobs_->item(row, 4)->setText(shown(timer.value(QStringLiteral("NextElapseUSecRealtime"))));
    }

    void show_log(int row) {
        if (row < 0 || row >= static_cast<int>(kServerJobs.size())) return;
        const ServerJob& j = kServerJobs[static_cast<std::size_t>(row)];
        const QString out = run(QStringLiteral("journalctl"),
            {QStringLiteral("-u"), QString::fromLatin1(j.service) + QStringLiteral(".service"), QStringLiteral("-n"),
             QStringLiteral("200"), QStringLiteral("--no-pager"), QStringLiteral("-o"), QStringLiteral("short-iso")}, 5000);
        detail_->setHtml(QStringLiteral("<h3>%1: last 200 lines</h3><pre>%2</pre>")
                             .arg(QString::fromLatin1(j.service).toHtmlEscaped(),
                                  out.isEmpty() ? QStringLiteral("(no log: not run yet, or this user cannot read the journal; "
                                                                 "add it to the systemd-journal group)")
                                                : out.toHtmlEscaped()));
    }

    void run_probe() {
        const QString exe = root_ + QStringLiteral("/build/linux/app/altair_latency_probe");
        if (!QFileInfo(exe).isExecutable()) {
            status_->setText(QStringLiteral("altair_latency_probe is not built: run ops/build.sh"));
            return;
        }
        if (probe_ != nullptr) return;
        probe_ = new QProcess(this);
        probe_->setWorkingDirectory(root_);
        probe_btn_->setEnabled(false);
        status_->setText(QStringLiteral("Probing: about 15 seconds..."));
        QObject::connect(probe_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            probe_->deleteLater();
            probe_ = nullptr;
            probe_btn_->setEnabled(true);
            detail_->setHtml(latency_html());
            status_->setText(code == 0 ? QStringLiteral("Probe done: report below and in data/verified/latency/")
                                       : QStringLiteral("The probe exited with code %1").arg(code));
        });
        probe_->start(exe, {});
    }

    void start_update() {
        const QStringList cmd = update_command(root_);
        if (QMessageBox::question(this, QStringLiteral("Install or update ALTAIR"),
                QStringLiteral("This opens a terminal that runs sudo ops/setup.sh (it asks for your password): system "
                               "packages, Qt, git pull of main, the build, and the scheduled jobs. It refuses during "
                               "market hours or while a job runs. Close ALTAIR afterwards and open it again to use the "
                               "new build.\n\nGo ahead?")) != QMessageBox::Yes)
            return;
        status_->setText(QProcess::startDetached(cmd.first(), cmd.mid(1), root_)
                             ? QStringLiteral("The update runs in its own terminal window.")
                             : QStringLiteral("Could not open a terminal (x-terminal-emulator). Run: sudo ops/setup.sh"));
    }

    QString root_;
    QTableWidget* jobs_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* probe_btn_ = nullptr;
    QPushButton* update_btn_ = nullptr;
    QProcess* probe_ = nullptr;
    bool shown_ = false;
};

} // namespace altair::ui
