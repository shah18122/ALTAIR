// P2-10 acceptance tests for the credential-free broker activity projection.

#include <desktop/broker_activity.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

QString fixture(qint64 observed = 1000, qint64 expires = 1100) {
    return QStringLiteral(R"json({
 "schema_version":1,
 "observed_at_unix":%1,
 "expires_at_unix":%2,
 "routes":{"data_primary":"FYERS","data_fallback":"KITE","order_primary":"KITE","mode":"PAPER","revision":7},
 "brokers":[
  {"broker":"FYERS","omitted_events":2,"events":[{"event_id":"1:1","occurred_at_unix":1002,"source":"account","kind":"auth","message":"session verified","correlation_id":"9:1"}]},
  {"broker":"KITE","omitted_events":1,"events":[{"event_id":"2:1","occurred_at_unix":1001,"source":"feed","kind":"error","message":"helper unavailable"}]}
 ]
})json").arg(observed).arg(expires);
}

QString write_fixture(const QDir& dir, const QString& name, const QString& body) {
    const QString path = dir.filePath(name);
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open projection fixture");
    const QByteArray bytes = body.toUtf8();
    check(file.write(bytes) == bytes.size(), "write projection fixture");
    return path;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory is available");
    if (!temporary.isValid()) return 1;
    const QDir dir(temporary.path());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1050, QTimeZone::UTC);
    const QString path = write_fixture(dir, QStringLiteral("activity.json"), fixture());

    const auto loaded = altair::ui::read_activity_projection(path, now);
    check(loaded.has_value(), "valid projection is accepted");
    if (loaded) {
        check(loaded->events.size() == 2, "both broker event streams are retained");
        check(loaded->events[0].broker == QStringLiteral("KITE"),
              "events are ordered by occurrence time");
        check(loaded->omitted_events == 3, "bounded retention overflow is visible");
        check(loaded->events[1].correlation_id == QStringLiteral("9:1"),
              "correlation identity survives parsing");
        const auto exported = altair::ui::export_activity(*loaded);
        check(exported.has_value() && exported->contains(QStringLiteral("FYERS")),
              "redacted export contains provider attribution");
        check(exported.has_value()
                  && !exported->contains(QStringLiteral("token"), Qt::CaseInsensitive)
                  && !exported->contains(QStringLiteral("authorization"), Qt::CaseInsensitive),
              "redacted export contains no credential markers");
    }

    const auto filtered = altair::ui::read_activity_projection(path, now);
    check(filtered.has_value(), "projection remains reusable for filtered views");

    const QString expired = write_fixture(dir, QStringLiteral("expired.json"), fixture(1000, 1049));
    check(!altair::ui::read_activity_projection(expired, now),
          "expired projection fails closed");

    const auto wall_now = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    const QString live_path = write_fixture(dir, QStringLiteral("live.json"),
        fixture(wall_now - 10, wall_now + 100));
    const QString secret = write_fixture(dir, QStringLiteral("secret.json"),
        fixture().replace(QStringLiteral("helper unavailable"), QStringLiteral("authorization leaked")));
    check(!altair::ui::read_activity_projection(secret, now),
          "secret-bearing event fails closed");

    altair::ui::BrokerActivityPanel panel(live_path);
    check(panel.projection_valid(), "activity panel accepts valid projection");
    check(panel.visible_event_count() == 2, "activity panel renders both providers");
    auto* filter = panel.findChild<QLineEdit*>(QStringLiteral("activityFilter"));
    check(filter != nullptr, "activity filter is discoverable");
    if (filter != nullptr) {
        filter->setText(QStringLiteral("FYERS"));
        check(panel.visible_event_count() == 1,
              "filter keeps only matching activity rows");
    }
    check(panel.redacted_export().contains(QStringLiteral("FYERS")),
          "panel export remains available after filtering");

    std::printf("Broker activity: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}