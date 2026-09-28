// desktop/fyers_panel.hpp -- read-only FYERS account snapshot viewer.
#pragma once

#include "account_widgets.hpp"
#include "helper_process.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace altair::ui {

class FyersAccountPanel final : public QWidget {
public:
    explicit FyersAccountPanel(QString snapshot_path,
                               std::function<void()> on_changed = {},
                               QWidget* parent = nullptr)
        : QWidget(parent), snapshot_(std::move(snapshot_path)),
          on_changed_(std::move(on_changed)) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        head_ = new QLabel(this);
        head_->setWordWrap(true);
        head_->setContentsMargins(14, 12, 14, 8);
        root->addWidget(head_);

        auto* action = new QWidget(this);
        auto* row = new QHBoxLayout(action);
        row->setContentsMargins(14, 4, 14, 8);
        fetch_ = new QPushButton(QStringLiteral("Fetch from FYERS"), action);
        reread_ = new QPushButton(QStringLiteral("Re-read file"), action);
        status_ = new QLabel(action);
        status_->setStyleSheet(QStringLiteral("color:#9FB3C8;"));
        row->addWidget(fetch_);
        row->addWidget(reread_);
        row->addWidget(status_, 1);
        root->addWidget(action);

        tabs_ = new QTabWidget(this);
        funds_ = table({QStringLiteral("Fund"), QStringLiteral("Equity"),
                        QStringLiteral("Commodity")});
        positions_ = json_page();
        holdings_ = json_page();
        orders_ = json_page();
        tabs_->addTab(funds_, QStringLiteral("Funds"));
        tabs_->addTab(positions_, QStringLiteral("Positions"));
        tabs_->addTab(holdings_, QStringLiteral("Holdings"));
        tabs_->addTab(orders_, QStringLiteral("Orders"));
        root->addWidget(tabs_, 1);

        connect(reread_, &QPushButton::clicked, this, [this] { reload(); });
        connect(fetch_, &QPushButton::clicked, this, [this] { fetch(); });
        reload();
    }

    /// Continue a successful OAuth link into the read-only account-data step.
    /// The helper contains only profile/funds/positions/holdings/orders GETs.
    void refresh_from_broker() { fetch(); }

    void reload_snapshot() { reload(); }
    [[nodiscard]] QString snapshot_summary() const { return head_->text(); }

private:
    [[nodiscard]] static QTableWidget* table(const QStringList& headers) {
        auto* out = new QTableWidget;
        out->setColumnCount(headers.size());
        out->setHorizontalHeaderLabels(headers);
        out->horizontalHeader()->setStretchLastSection(true);
        out->verticalHeader()->setVisible(false);
        out->setEditTriggers(QAbstractItemView::NoEditTriggers);
        out->setSelectionBehavior(QAbstractItemView::SelectRows);
        return out;
    }

    [[nodiscard]] static QPlainTextEdit* json_page() {
        auto* out = new QPlainTextEdit;
        out->setReadOnly(true);
        out->setStyleSheet(QStringLiteral(
            "QPlainTextEdit{background:#11171C;color:#D6DBDF;"
            "font-family:Consolas,monospace;font-size:12px;border:none;}"));
        return out;
    }

    [[nodiscard]] static QString amount(const QJsonValue& value) {
        if (!value.isDouble()) return QStringLiteral("—");
        return QStringLiteral("₹%1").arg(value.toDouble(), 0, 'f', 2);
    }

    [[nodiscard]] static QString section_text(const QJsonObject& root,
                                              const QString& name) {
        const int status = root.value(name + QStringLiteral("_status")).toInt();
        const QString state = root.value(name + QStringLiteral("_state")).toString();
        const QJsonValue payload = root.value(name);
        if (status != 200 || state != QStringLiteral("present")
            || payload.isNull() || payload.isUndefined()) {
            return QStringLiteral("NOT AVAILABLE · %1 · HTTP %2\n\n"
                                  "Absent is not an empty account section.")
                .arg(state.isEmpty() ? QStringLiteral("unknown state") : state,
                     QString::number(status));
        }
        if (payload.isObject())
            return QString::fromUtf8(QJsonDocument(payload.toObject())
                                         .toJson(QJsonDocument::Indented));
        if (payload.isArray())
            return QString::fromUtf8(QJsonDocument(payload.toArray())
                                         .toJson(QJsonDocument::Indented));
        return QStringLiteral("INVALID SNAPSHOT SECTION");
    }

    void clear(const QString& message) {
        head_->setText(message);
        funds_->setRowCount(0);
        for (auto* page : {positions_, holdings_, orders_}) page->clear();
    }

    void reload() {
        QFile file(snapshot_);
        if (!file.open(QIODevice::ReadOnly)) {
            clear(QStringLiteral(
                "<b style='color:#B9770B'>NO FYERS SNAPSHOT</b> — %1<br>"
                "Link FYERS, then press <b>Fetch from FYERS</b>. The helper "
                "uses read-only profile, funds, positions, holdings and order-book GETs.")
                      .arg(snapshot_.toHtmlEscaped()));
            return;
        }
        constexpr qint64 kMaxSnapshotBytes = 8 * 1024 * 1024;
        const QByteArray raw = file.read(kMaxSnapshotBytes + 1);
        if (raw.size() > kMaxSnapshotBytes) {
            clear(QStringLiteral("<b style='color:#F85149'>FYERS SNAPSHOT TOO LARGE</b>"));
            return;
        }
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            clear(QStringLiteral("<b style='color:#F85149'>INVALID FYERS SNAPSHOT</b> — %1")
                      .arg(error.errorString().toHtmlEscaped()));
            return;
        }
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("schema_version")).toInt() != 1
            || root.value(QStringLiteral("broker")).toString()
                   != QStringLiteral("FYERS")) {
            clear(QStringLiteral(
                "<b style='color:#F85149'>WRONG FYERS SNAPSHOT SCHEMA</b>"));
            return;
        }
        const qint64 at = static_cast<qint64>(
            root.value(QStringLiteral("fetched_at_unix")).toDouble());
        const QDateTime when = QDateTime::fromSecsSinceEpoch(at, QTimeZone::UTC);
        const qint64 raw_age = when.isValid()
            ? when.secsTo(QDateTime::currentDateTimeUtc()) : -1;
        const qint64 age = raw_age >= 0 ? raw_age : -1;
        const QString account = root.value(QStringLiteral("account_id")).toString();
        if (at <= 0 || account.isEmpty()) {
            clear(QStringLiteral(
                "<b style='color:#F85149'>FYERS SNAPSHOT IDENTITY MISSING</b>"));
            return;
        }
        const bool complete = root.value(QStringLiteral("snapshot_complete")).toBool(false);
        constexpr qint64 kFreshSeconds = 30;
        const bool fresh = age >= 0 && age < kFreshSeconds;
        const QString completion = complete ? QStringLiteral("COMPLETE")
                                            : QStringLiteral("PARTIAL");
        const QString freshness = fresh ? QStringLiteral("FRESH")
                                        : QStringLiteral("STALE");
        head_->setText(QStringLiteral(
            "<b>FYERS ACCOUNT · %1 · %2</b> · %3 · snapshot age %4")
            .arg(completion, freshness,
                 account.isEmpty() ? QStringLiteral("UNKNOWN") : account.toHtmlEscaped(),
                 age < 0 ? QStringLiteral("UNKNOWN")
                         : QStringLiteral("%1 seconds").arg(age)));

        funds_->setRowCount(0);
        const QJsonObject funds = root.value(QStringLiteral("funds")).toObject();
        const QJsonArray limits = funds.value(QStringLiteral("fund_limit")).toArray();
        for (const QJsonValue& value : limits) {
            const QJsonObject item = value.toObject();
            const int row = funds_->rowCount();
            funds_->insertRow(row);
            funds_->setItem(row, 0, new QTableWidgetItem(
                item.value(QStringLiteral("title")).toString(
                    QStringLiteral("Fund %1").arg(item.value(QStringLiteral("id")).toInt()))));
            funds_->setItem(row, 1, new QTableWidgetItem(
                amount(item.value(QStringLiteral("equityAmount")))));
            funds_->setItem(row, 2, new QTableWidgetItem(
                amount(item.value(QStringLiteral("commodityAmount")))));
        }
        if (limits.isEmpty()) {
            funds_->setRowCount(1);
            funds_->setSpan(0, 0, 1, 3);
            funds_->setItem(0, 0, new QTableWidgetItem(
                root.value(QStringLiteral("funds_status")).toInt() == 200
                    && root.value(QStringLiteral("funds_state")).toString()
                           == QStringLiteral("present")
                    ? QStringLiteral("FYERS returned no fund_limit rows")
                    : QStringLiteral("Funds were not fetched; this is absent, not zero")));
        }
        positions_->setPlainText(section_text(root, QStringLiteral("positions")));
        holdings_->setPlainText(section_text(root, QStringLiteral("holdings")));
        orders_->setPlainText(section_text(root, QStringLiteral("orders")));
        status_->clear();
    }

    void fetch() {
        const QString executable =
#if defined(_WIN32)
            QStringLiteral("altair_fyers_account.exe");
#else
            QStringLiteral("altair_fyers_account");
#endif
        QStringList candidates;
        candidates << QCoreApplication::applicationDirPath()
                          + QStringLiteral("/../app/") + executable;
        candidates << QCoreApplication::applicationDirPath()
                          + QStringLiteral("/../../net/app/") + executable;
#ifdef ALTAIR_SOURCE_DIR
        candidates << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/")
                          + executable;
#endif
        QString found;
        for (const QString& candidate : candidates) {
            if (QFileInfo(candidate).isFile()) {
                found = QFileInfo(candidate).canonicalFilePath();
                break;
            }
        }
        if (found.isEmpty()) {
            status_->setText(QStringLiteral(
                "FYERS account helper is not built; build the net preset."));
            return;
        }
        fetch_->setEnabled(false);
        status_->setText(QStringLiteral("fetching read-only account snapshot..."));
        QString working_directory;
#ifdef ALTAIR_SOURCE_DIR
        working_directory = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        const auto generation = helper_.start(
            found,
            {QStringLiteral("--go"), QStringLiteral("--out"), snapshot_},
            working_directory, 60000, {},
            [this](HelperProcessResult result) {
                fetch_->setEnabled(true);
                if (!result.ran_to_completion()) {
                    status_->setText(QStringLiteral("FYERS fetch failed: %1")
                                         .arg(result.detail));
                    return;
                }
                if (result.exit_code != 0) {
                    status_->setText(QStringLiteral("FYERS fetch failed: %1")
                        .arg(result.output.trimmed().section(QChar('\n'), -2)));
                    return;
                }
                reload();
                if (on_changed_) on_changed_();
            });
        if (!generation) {
            fetch_->setEnabled(true);
            status_->setText(QStringLiteral("another FYERS account fetch is still running"));
        }
    }

    QString snapshot_;
    std::function<void()> on_changed_;
    QLabel* head_{};
    QLabel* status_{};
    QPushButton* fetch_{};
    QPushButton* reread_{};
    QTabWidget* tabs_{};
    QTableWidget* funds_{};
    QPlainTextEdit* positions_{};
    QPlainTextEdit* holdings_{};
    QPlainTextEdit* orders_{};
    // Destroy first so callbacks are suppressed before widget members disappear.
    HelperProcess helper_;
};

} // namespace altair::ui
