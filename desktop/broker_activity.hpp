// desktop/broker_activity.hpp -- bounded, credential-free broker activity view.
//
// The service owns credentials and network transport.  The desktop consumes only
// this small JSON projection: route metadata, redacted events and an overflow
// count.  A missing, malformed, provider-incomplete or expired projection is
// displayed as unavailable; it is never promoted to a connected state.
#pragma once

#include <types/broker_log.hpp>

#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QTimeZone>
#include <QVector>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace altair::ui {

inline constexpr qsizetype kMaxBrokerActivityFileBytes = 256 * 1024;
inline constexpr int kMaxBrokerActivityEvents = 256;
inline constexpr qsizetype kMaxBrokerActivityExportBytes = 128 * 1024;

enum class ActivityError : std::uint8_t {
    FileMissing, FileTooLarge, Malformed, WrongSchema, Expired, Sensitive,
    TooManyEvents, InvalidId, ExportTooLarge
};

struct ActivityEvent {
    QString broker;
    QString source;
    QString kind;
    QString event_id;
    QString correlation_id;
    QDateTime occurred_at;
    QString message;
};

struct ActivityProjection {
    std::uint32_t schema_version{};
    QDateTime observed_at;
    QDateTime expires_at;
    QString data_primary;
    QString data_fallback;
    QString order_primary;
    QString mode;
    std::uint64_t route_revision{};
    std::uint64_t omitted_events{};
    QVector<ActivityEvent> events;

    [[nodiscard]] bool has_broker(QStringView broker) const noexcept {
        return std::any_of(events.cbegin(), events.cend(),
                           [broker](const ActivityEvent& event) {
                               return event.broker == broker;
                           });
    }
};

[[nodiscard]] inline QString activity_broker_name(broker_view::BrokerId broker) {
    switch (broker) {
    case broker_view::BrokerId::Fyers: return QStringLiteral("FYERS");
    case broker_view::BrokerId::ZerodhaKite: return QStringLiteral("KITE");
    case broker_view::BrokerId::None: break;
    }
    return QStringLiteral("UNKNOWN");
}

[[nodiscard]] inline bool safe_activity_text(const QString& text,
                                             int max_length = 192) noexcept {
    if (text.isEmpty() || text.size() > max_length) return false;
    const auto checked = broker_view::make_redacted_message(text.toUtf8().toStdString());
    return checked.has_value();
}

[[nodiscard]] inline std::expected<QString, ActivityError>
parse_activity_id(const QJsonValue& value) {
    QString text;
    if (value.isString()) {
        text = value.toString();
        const auto parts = text.split(QLatin1Char(':'));
        bool hi_ok = false;
        bool lo_ok = false;
        if (parts.size() == 2) {
            const auto hi = parts[0].toULongLong(&hi_ok, 10);
            const auto lo = parts[1].toULongLong(&lo_ok, 10);
            if (hi_ok && lo_ok && (hi != 0 || lo != 0)) return text;
        }
    } else if (value.isObject()) {
        const auto object = value.toObject();
        bool hi_ok = false;
        bool lo_ok = false;
        const auto hi = QString::number(object.value(QStringLiteral("high")).toVariant().toULongLong(&hi_ok));
        const auto lo = QString::number(object.value(QStringLiteral("low")).toVariant().toULongLong(&lo_ok));
        if (hi_ok && lo_ok && (hi != QStringLiteral("0") || lo != QStringLiteral("0")))
            return hi + QLatin1Char(':') + lo;
    }
    return std::unexpected(ActivityError::InvalidId);
}

[[nodiscard]] inline std::expected<ActivityProjection, ActivityError>
read_activity_projection(const QString& path,
                         const QDateTime& now = QDateTime::currentDateTimeUtc()) {
    QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return std::unexpected(ActivityError::FileMissing);
    if (info.size() <= 0 || info.size() > kMaxBrokerActivityFileBytes)
        return std::unexpected(ActivityError::FileTooLarge);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(ActivityError::FileMissing);
    const QByteArray raw = file.read(kMaxBrokerActivityFileBytes + 1);
    if (raw.size() > kMaxBrokerActivityFileBytes)
        return std::unexpected(ActivityError::FileTooLarge);
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return std::unexpected(ActivityError::Malformed);

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != 1)
        return std::unexpected(ActivityError::WrongSchema);
    const qint64 observed = static_cast<qint64>(
        root.value(QStringLiteral("observed_at_unix")).toDouble());
    const qint64 expires = static_cast<qint64>(
        root.value(QStringLiteral("expires_at_unix")).toDouble());
    ActivityProjection result;
    result.schema_version = 1;
    result.observed_at = QDateTime::fromSecsSinceEpoch(observed, QTimeZone::UTC);
    result.expires_at = QDateTime::fromSecsSinceEpoch(expires, QTimeZone::UTC);
    if (!result.observed_at.isValid() || !result.expires_at.isValid()
        || observed <= 0 || expires <= observed || now < result.observed_at
        || now >= result.expires_at)
        return std::unexpected(ActivityError::Expired);

    const QJsonObject routes = root.value(QStringLiteral("routes")).toObject();
    result.data_primary = routes.value(QStringLiteral("data_primary")).toString();
    result.data_fallback = routes.value(QStringLiteral("data_fallback")).toString();
    result.order_primary = routes.value(QStringLiteral("order_primary")).toString();
    result.mode = routes.value(QStringLiteral("mode")).toString();
    result.route_revision = routes.value(QStringLiteral("revision")).toVariant().toULongLong();
    if (result.route_revision == 0 || result.mode.isEmpty()
        || (result.data_primary != QStringLiteral("FYERS")
            && result.data_primary != QStringLiteral("KITE"))
        || (result.data_fallback != QStringLiteral("FYERS")
            && result.data_fallback != QStringLiteral("KITE"))
        || (result.order_primary != QStringLiteral("FYERS")
            && result.order_primary != QStringLiteral("KITE")))
        return std::unexpected(ActivityError::Malformed);

    const QJsonArray brokers = root.value(QStringLiteral("brokers")).toArray();
    if (brokers.size() != 2) return std::unexpected(ActivityError::Malformed);
    for (const QJsonValue& broker_value : brokers) {
        const QJsonObject broker = broker_value.toObject();
        const QString provider = broker.value(QStringLiteral("broker")).toString();
        if (provider != QStringLiteral("FYERS") && provider != QStringLiteral("KITE"))
            return std::unexpected(ActivityError::Malformed);
        result.omitted_events += broker.value(QStringLiteral("omitted_events"))
                                      .toVariant().toULongLong();
        const QJsonArray events = broker.value(QStringLiteral("events")).toArray();
        if (result.events.size() + events.size() > kMaxBrokerActivityEvents)
            return std::unexpected(ActivityError::TooManyEvents);
        for (const QJsonValue& event_value : events) {
            const QJsonObject event = event_value.toObject();
            const auto id = parse_activity_id(event.value(QStringLiteral("event_id")));
            if (!id) return std::unexpected(id.error());
            QString correlation;
            if (!event.value(QStringLiteral("correlation_id")).isUndefined()
                && !event.value(QStringLiteral("correlation_id")).isNull()) {
                const auto parsed = parse_activity_id(event.value(QStringLiteral("correlation_id")));
                if (!parsed) return std::unexpected(parsed.error());
                correlation = *parsed;
            }
            const QString source = event.value(QStringLiteral("source")).toString();
            const QString kind = event.value(QStringLiteral("kind")).toString();
            const QString message = event.value(QStringLiteral("message")).toString();
            const qint64 occurred = static_cast<qint64>(
                event.value(QStringLiteral("occurred_at_unix")).toDouble());
            if (!safe_activity_text(source, 64) || !safe_activity_text(kind, 32)
                || !safe_activity_text(message) || occurred <= 0) {
                return std::unexpected(ActivityError::Sensitive);
            }
            ActivityEvent parsed;
            parsed.broker = provider;
            parsed.source = source;
            parsed.kind = kind;
            parsed.event_id = *id;
            parsed.correlation_id = correlation;
            parsed.occurred_at = QDateTime::fromSecsSinceEpoch(occurred, QTimeZone::UTC);
            parsed.message = message;
            result.events.push_back(std::move(parsed));
        }
    }
    std::sort(result.events.begin(), result.events.end(),
              [](const ActivityEvent& a, const ActivityEvent& b) {
                  return a.occurred_at < b.occurred_at;
              });
    if (!result.has_broker(QStringLiteral("FYERS"))
        || !result.has_broker(QStringLiteral("KITE")))
        return std::unexpected(ActivityError::Malformed);
    return result;
}

[[nodiscard]] inline std::expected<QString, ActivityError>
export_activity(const ActivityProjection& projection, const QString& filter = {}) {
    QString out = QStringLiteral(
        "Altair broker activity (redacted)\n"
        "Data: %1 -> %2   Orders: %3   Mode: %4   Route revision: %5\n"
        "Omitted events: %6\n")
        .arg(projection.data_primary, projection.data_fallback,
             projection.order_primary, projection.mode)
        .arg(projection.route_revision).arg(projection.omitted_events);
    for (const ActivityEvent& event : projection.events) {
        const QString line = QStringLiteral("%1 | %2 | %3 | event %4 | correlation %5 | %6\n")
            .arg(event.occurred_at.toString(Qt::ISODate), event.broker,
                 event.source, event.event_id,
                 event.correlation_id.isEmpty() ? QStringLiteral("—") : event.correlation_id,
                 event.message);
        if (!filter.isEmpty() && !line.contains(filter, Qt::CaseInsensitive)) continue;
        out += line;
    }
    if (out.toUtf8().size() > kMaxBrokerActivityExportBytes)
        return std::unexpected(ActivityError::ExportTooLarge);
    return out;
}

class BrokerActivityPanel final : public QWidget {
public:
    explicit BrokerActivityPanel(QString path, QWidget* parent = nullptr)
        : QWidget(parent), path_(std::move(path)) {
        auto* root = new QVBoxLayout(this);
        auto* routes = new QHBoxLayout;
        routes->addWidget(new QLabel(QStringLiteral("Data"), this));
        data_ = new QComboBox(this);
        data_->addItems({QStringLiteral("FYERS"), QStringLiteral("KITE")});
        routes->addWidget(data_);
        routes->addWidget(new QLabel(QStringLiteral("Orders"), this));
        orders_ = new QComboBox(this);
        orders_->addItems({QStringLiteral("KITE"), QStringLiteral("FYERS")});
        routes->addWidget(orders_);
        route_note_ = new QLabel(this);
        route_note_->setStyleSheet(QStringLiteral("color:#9FB3C8;"));
        routes->addWidget(route_note_, 1);
        root->addLayout(routes);

        auto* actions = new QHBoxLayout;
        filter_ = new QLineEdit(this);
        filter_->setPlaceholderText(QStringLiteral("Filter broker, source, event, correlation or message"));
        filter_->setObjectName(QStringLiteral("activityFilter"));
        actions->addWidget(filter_, 1);
        reload_ = new QPushButton(QStringLiteral("Refresh"), this);
        copy_ = new QPushButton(QStringLiteral("Copy redacted export"), this);
        save_ = new QPushButton(QStringLiteral("Save export"), this);
        actions->addWidget(reload_);
        actions->addWidget(copy_);
        actions->addWidget(save_);
        root->addLayout(actions);

        status_ = new QLabel(this);
        status_->setWordWrap(true);
        root->addWidget(status_);
        table_ = new QTableWidget(0, 6, this);
        table_->setObjectName(QStringLiteral("brokerActivityTable"));
        table_->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Broker"),
            QStringLiteral("Source"), QStringLiteral("Kind"), QStringLiteral("Event / correlation"),
            QStringLiteral("Redacted message")});
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setAlternatingRowColors(true);
        table_->horizontalHeader()->setStretchLastSection(true);
        root->addWidget(table_, 1);

        connect(filter_, &QLineEdit::textChanged, this, [this] { render(); });
        connect(reload_, &QPushButton::clicked, this, [this] { reload(); });
        connect(copy_, &QPushButton::clicked, this, [this] { copy_export(); });
        connect(save_, &QPushButton::clicked, this, [this] { save_export(); });
        connect(data_, &QComboBox::currentTextChanged, this, [this] { route_note_->setText(
            QStringLiteral("Local preference only; service route remains %1 (rev %2).")
                .arg(projection_.data_primary).arg(projection_.route_revision)); });
        timer_ = new QTimer(this);
        timer_->setInterval(5000);
        connect(timer_, &QTimer::timeout, this, [this] { reload(); });
        timer_->start();
        reload();
    }

    void reload() {
        const auto loaded = read_activity_projection(path_);
        if (!loaded) {
            valid_ = false;
            table_->setRowCount(0);
            status_->setText(QStringLiteral("Broker activity unavailable: service projection is missing, malformed or expired. "
                                            "No broker state is inferred."));
            route_note_->setText(QStringLiteral("No verified service route."));
            return;
        }
        projection_ = *loaded;
        valid_ = true;
        data_->setCurrentText(projection_.data_primary);
        orders_->setCurrentText(projection_.order_primary);
        route_note_->setText(QStringLiteral("Service route revision %1; local controls do not enable live orders.")
                                 .arg(projection_.route_revision));
        status_->setText(QStringLiteral("%1 events shown from both brokers · %2 omitted by bounded service retention · "
                                        "projection expires %3")
                         .arg(projection_.events.size()).arg(projection_.omitted_events)
                         .arg(projection_.expires_at.toString(Qt::ISODate)));
        render();
    }

    [[nodiscard]] bool projection_valid() const noexcept { return valid_; }
    [[nodiscard]] int visible_event_count() const noexcept { return table_->rowCount(); }
    [[nodiscard]] QString redacted_export() const {
        if (!valid_) return {};
        const auto result = export_activity(projection_, filter_->text());
        return result ? *result : QString{};
    }

private:
    void render() {
        table_->setRowCount(0);
        if (!valid_) return;
        const QString needle = filter_->text();
        for (const ActivityEvent& event : projection_.events) {
            const QString searchable = event.broker + QLatin1Char(' ') + event.source
                + QLatin1Char(' ') + event.kind + QLatin1Char(' ') + event.event_id
                + QLatin1Char(' ') + event.correlation_id + QLatin1Char(' ') + event.message;
            if (!needle.isEmpty() && !searchable.contains(needle, Qt::CaseInsensitive)) continue;
            const int row = table_->rowCount();
            table_->insertRow(row);
            const QString id = event.event_id + QStringLiteral(" / ")
                + (event.correlation_id.isEmpty() ? QStringLiteral("—") : event.correlation_id);
            const QStringList values{event.occurred_at.toString(Qt::ISODate), event.broker,
                                     event.source, event.kind, id, event.message};
            for (int column = 0; column < values.size(); ++column)
                table_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }

    void copy_export() {
        const QString text = redacted_export();
        if (text.isEmpty()) { status_->setText(QStringLiteral("Nothing to export: projection is unavailable or too large.")); return; }
        QApplication::clipboard()->setText(text);
        status_->setText(QStringLiteral("Copied bounded, redacted activity export."));
    }

    void save_export() {
        const QString text = redacted_export();
        if (text.isEmpty()) { status_->setText(QStringLiteral("Nothing to export: projection is unavailable or too large.")); return; }
        const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("Save redacted broker activity"),
                                                                  QStringLiteral("broker_activity.txt"),
                                                                  QStringLiteral("Text files (*.txt)"));
        if (destination.isEmpty()) return;
        QFile file(destination);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(text.toUtf8()) != text.toUtf8().size()) {
            status_->setText(QStringLiteral("Could not write the redacted export."));
            return;
        }
        status_->setText(QStringLiteral("Saved bounded, redacted activity export."));
    }

    QString path_;
    ActivityProjection projection_{};
    bool valid_{};
    QComboBox* data_{};
    QComboBox* orders_{};
    QLabel* route_note_{};
    QLineEdit* filter_{};
    QPushButton* reload_{};
    QPushButton* copy_{};
    QPushButton* save_{};
    QLabel* status_{};
    QTableWidget* table_{};
    QTimer* timer_{};
};

} // namespace altair::ui