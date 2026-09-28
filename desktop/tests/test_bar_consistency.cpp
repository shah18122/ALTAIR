#include "../data/bar_consistency_io.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

namespace {
namespace bc = altair::ui::barcheck;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

qint64 start() {
    return QDateTime::fromString("2026-09-01T09:15:00+05:30", Qt::ISODate).toSecsSinceEpoch();
}

bc::Series fixture(int minutes, int day_offset = 0) {
    bc::Series result;
    for (int at = 0; at < 375; at += minutes) {
        const int end = std::min(at + minutes, 375);
        bc::Bar bar;
        bar.start = start() + day_offset * 86400LL + at * 60LL;
        bar.price = {10000 + at * 100, 10000 + (end - 1) * 100 + 14,
                     10000 + at * 100 - 10, 10000 + (end - 1) * 100 + 2};
        bar.volume = static_cast<qint64>(at + 1 + end) * (end - at) / 2;
        result.emplace(bar.start, bar);
    }
    return result;
}

QByteArray rupees(qint64 paise) {
    return QByteArray::number(paise / 100) + '.' + QByteArray::number(paise % 100).rightJustified(2, '0');
}

QByteArray csv(const bc::Series& series) {
    QByteArray result("time,open,high,low,close,volume\n");
    for (const auto& [key, bar] : series) {
        result += bc::detail::stamp(key).toLatin1();
        for (const qint64 price : bar.price) result += ',' + rupees(price);
        result += ',';
        if (bar.volume) result += QByteArray::number(*bar.volume);
        result += '\n';
    }
    return result;
}

bool write(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void test_five_minute_conservation() {
    const auto base = fixture(1);
    auto target = fixture(5);
    const auto& first = target.begin()->second;
    check(first.price == std::array<qint64, 4>{10000, 10414, 9990, 10402} &&
          first.volume == 15, "independent five-minute expected numeric values");
    bc::Report report;
    bc::compare(base, target, 1, 5, report);
    check(report.ok() && report.matched_windows == 75 && report.prices_compared == 300 &&
          report.volumes_compared == 75, "375 one-minute bars reconcile to 75 five-minute bars");
    ++target.begin()->second.price[1];
    bc::Report mismatch;
    bc::compare(base, target, 1, 5, mismatch);
    check(!mismatch.ok() && mismatch.counts.value("price_mismatch") == 1 &&
          mismatch.matched_windows == 74, "one-paise high mismatch cannot pass");
}

void test_session_anchor_and_short_tail() {
    bc::Report report;
    const auto target = fixture(60);
    bc::compare(fixture(1), target, 1, 60, report);
    check(report.ok() && report.matched_windows == 7, "09:15 hourly anchor and truncated final bar");
    check(target.rbegin()->first == start() + 360 * 60 && target.rbegin()->second.volume == 5520,
          "15:15 tail contains 15 bars with volume sum 361..375 = 5520");
    bc::Report base_five;
    bc::compare(fixture(5), target, 5, 60, base_five);
    check(base_five.ok(), "five-minute basis also conserves hourly tail");
    bc::Report base_tail;
    bc::compare(fixture(60), fixture(120), 60, 120, base_tail);
    check(base_tail.ok() && base_tail.matched_windows == 4, "truncated base bar in final target window");
}

void test_gap_and_duplicate_refusal() {
    auto base = fixture(1);
    auto target = fixture(5);
    base.erase(start() + 60);
    target.erase(start() + 300);
    bc::Report report;
    bc::compare(base, target, 1, 5, report);
    check(!report.ok() && report.counts.value("missing_base") == 1 &&
          report.counts.value("missing_target") == 1, "missing base and target count exactly");
    QTemporaryDir dir;
    check(dir.isValid(), "temporary fixture directory");
    const QString path = dir.filePath("duplicate.csv");
    const QByteArray one = csv(bc::Series{{start(), fixture(1).begin()->second}});
    check(write(path, one + one.mid(one.indexOf('\n') + 1)), "write duplicate fixture");
    bc::Report duplicate;
    const auto loaded = bc::load(path, 1, false, false, duplicate);
    check(loaded.size() == 1 && duplicate.counts.value("duplicate_timestamp") == 1,
          "duplicate timestamp visibly refused");
    auto misaligned = fixture(5);
    auto node = misaligned.extract(start());
    node.key() += 60;
    node.mapped().start += 60;
    misaligned.insert(std::move(node));
    bc::Report alignment;
    bc::compare(fixture(1), misaligned, 1, 5, alignment);
    check(!alignment.ok() && alignment.counts.value("misaligned_timestamp") == 1,
          "epoch-style offset is not accepted as session alignment");
}

void test_volume_semantics_and_overflow() {
    auto base = fixture(1);
    auto target = fixture(5);
    for (int i = 0; i < 5; ++i) base.at(start() + i * 60).volume.reset();
    target.begin()->second.volume.reset();
    bc::Report unknown;
    bc::compare(base, target, 1, 5, unknown);
    check(unknown.ok() && unknown.volumes_unverified == 1 && unknown.volumes_compared == 74,
          "unknown/unknown is counted unverified rather than false equality");
    check(unknown.json().contains("ohlcv_fully_verified") &&
          !unknown.json().value("ohlcv_fully_verified").toBool(),
          "missing volumes cannot claim full OHLCV verification");
    target.begin()->second.volume = 0;
    bc::Report availability;
    bc::compare(base, target, 1, 5, availability);
    check(!availability.ok() && availability.counts.value("volume_availability_mismatch") == 1,
          "unknown must not equal genuine zero");
    for (int i = 0; i < 5; ++i) base.at(start() + i * 60).volume = 0;
    bc::Report zero;
    bc::compare(base, target, 1, 5, zero);
    check(zero.ok() && zero.volumes_compared == 75, "known zero conserves exactly");
    base.at(start()).volume = std::numeric_limits<qint64>::max();
    base.at(start() + 60).volume = 1;
    bc::Report overflow;
    bc::compare(base, target, 1, 5, overflow);
    check(!overflow.ok() && overflow.counts.value("volume_overflow") == 1, "int64 volume sum refuses overflow");
}

void test_numeric_limits() {
    qint64 value = 0;
    bool rounded = false;
    check(bc::parse_price("92233720368547758.07", false, value, rounded) &&
          value == std::numeric_limits<qint64>::max() && !rounded, "maximum int64 paise accepted exactly");
    for (const QByteArray bad : {"92233720368547758.08", "9999999999999999999999999",
                                "NaN", "inf", "1e3", "-1", "+1", "1..2", "", "1.", ".5", "0"})
        check(!bc::parse_price(bad, false, value, rounded), "unsafe decimal refused");
    check(!bc::parse_price("1.234", false, value, rounded), "default rejects subpaise rounding");
    check(bc::parse_price("1.234", true, value, rounded) && value == 123 && rounded, "explicit round down counted");
    check(bc::parse_price("1.235", true, value, rounded) && value == 124 && rounded, "explicit half-up counted");
    check(bc::parse_price("1.2300", false, value, rounded) && value == 123 && !rounded, "trailing zeroes exact");
    check(!bc::parse_price("92233720368547758.075", true, value, rounded), "rounding overflow refused");
    check(bc::parse_volume("9223372036854775807", value) && value == std::numeric_limits<qint64>::max(),
          "maximum volume parsed exactly");
    for (const QByteArray bad : {"9223372036854775808", "-1", "+1", "1.5", "NaN", ""})
        check(!bc::parse_volume(bad, value), "invalid volume refused");
}

void test_reader_schema_and_bounds() {
    QTemporaryDir dir;
    check(dir.isValid(), "reader temporary directory");
    const QString path = dir.filePath("input.csv");
    const QByteArray header("time,open,high,low,close,volume\n");
    const QByteArray time("2026-09-01T09:15:00+05:30,");
    const std::array<std::pair<QByteArray, const char*>, 8> bad{{
        {"date,open,high,low,close,volume\n", "invalid_schema"},
        {header + "2026-09-01T09:15:00Z,1,1,1,1,0\n", "invalid_timestamp"},
        {header + "2026-02-30T09:15:00+05:30,1,1,1,1,0\n", "invalid_timestamp"},
        {header + time + "2,1,1,2,0\n", "invalid_ohlc"},
        {header + time + "1,1,1,1,-1\n", "invalid_volume"},
        {header + QByteArray(5000, '0') + '\n', "line_limit"},
        {"time,open,high,low,close,volume,oi\n" + time + "1,1,1,1,0," +
            QByteArray(5000, '9') + '\n', "line_limit"},
        {header + time + "1,1,1,1\n", "invalid_row"}
    }};
    for (const auto& [contents, code] : bad) {
        check(write(path, contents), "write malformed fixture");
        bc::Report report;
        const auto loaded = bc::load(path, 1, false, false, report);
        check(loaded.empty() && report.counts.value(code) == 1, "malformed CSV refused with diagnostic");
    }
    bc::Report missing;
    check(bc::load(dir.filePath("absent.csv"), 1, false, false, missing).empty() &&
          missing.counts.value("io_error") == 1, "missing path refused");
    bc::Report capped;
    capped.detail_limit = 2;
    for (int i = 0; i < 5; ++i) capped.issue("test", "bounded");
    check(capped.diagnostics.size() == 2 && capped.omitted_details == 3 && capped.counts["test"] == 5,
          "diagnostic cap cannot hide aggregate errors");
    check(write(path, header + time + "1.235,1.235,1.235,1.235,0\n"), "write rounding fixture");
    bc::Report rounding;
    const auto rounded = bc::load(path, 1, true, true, rounding);
    check(rounded.size() == 1 && rounding.rounded_fields == 4 && !rounded.begin()->second.volume,
          "explicit rounding and zero-as-absent policy reported");
    qint64 rows = 2000000;
    bc::Series empty;
    bc::Report bounded;
    check(!bc::detail::read_file(path, 1, true, false, bounded, empty, rows) &&
          empty.empty() && bounded.counts.value("row_limit") == 1, "row capacity refuses excess input");
    bc::Series dates;
    for (int i = 0; i < 20001; ++i) {
        auto bar = fixture(375).begin()->second;
        bar.start += i * 86400LL;
        dates.emplace(bar.start, bar);
    }
    bc::Report too_many_dates;
    bc::compare(dates, {}, 1, 5, too_many_dates);
    check(too_many_dates.counts.value("session_limit") == 1 && too_many_dates.compared_windows == 0,
          "session-date capacity refuses excessive comparison");
}

void test_directory_and_missing_sessions() {
    QTemporaryDir dir;
    check(dir.isValid(), "directory fixture");
    const auto base = fixture(1);
    const QByteArray original = csv(base);
    check(write(dir.filePath("a.csv"), original) && write(dir.filePath("b.csv"), original), "write duplicate files");
    bc::Report report;
    const auto loaded = bc::load(dir.path(), 1, false, false, report);
    check(loaded.size() == 375 && report.counts.value("duplicate_timestamp") == 375,
          "directory duplicates not silently merged");
    check(read(dir.filePath("a.csv")) == original && read(dir.filePath("b.csv")) == original,
          "source bytes unchanged after reconciliation load");
    auto target = fixture(5);
    const auto next = fixture(5, 1);
    target.insert(next.begin(), next.end());
    bc::Report absent;
    bc::compare(base, target, 1, 5, absent);
    check(!absent.ok() && absent.session_dates == 2 && absent.counts.value("missing_base") == 375,
          "entire session missing on one side is incomplete");
}

void test_cli_contract() {
    bc::Report invalid;
    bc::compare(fixture(1), fixture(5), 2, 5, invalid);
    check(!invalid.ok() && invalid.counts.value("invalid_duration") == 1, "nonmultiple duration rejected");
    bc::Report empty;
    bc::compare({}, {}, 1, 5, empty);
    check(!empty.ok() && empty.counts.value("empty_input") == 1, "empty cannot report consistency");
    auto partial = fixture(1);
    partial.erase(start() + 374 * 60);
    bc::Report incomplete;
    bc::compare(partial, fixture(5), 1, 5, incomplete);
    check(!incomplete.ok() && incomplete.counts.value("missing_base") == 1, "latest partial session cannot pass");
    const QString scope = empty.json().value("scope").toString();
    check(scope.contains("calendar") && scope.contains("contract") && scope.contains("09:15"), "JSON scope limitations explicit");
    QTemporaryDir dir;
    const QString base_path = dir.filePath("base.csv");
    const QString target_path = dir.filePath("target.csv");
    check(write(base_path, csv(fixture(1))) && write(target_path, csv(fixture(5))), "write CLI fixtures");
    QString binary = QCoreApplication::applicationDirPath() + "/altair_bar_consistency";
#ifdef Q_OS_WIN
    binary += ".exe";
#endif
    const QStringList args{"--base", base_path, "--target", target_path,
                           "--base-minutes", "1", "--target-minutes", "5"};
    auto run = [&](QStringList arguments, int code) {
        QProcess process;
        process.start(binary, arguments);
        const bool finished = process.waitForStarted(10000) && process.waitForFinished(10000);
        check(finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == code,
              "CLI documented exit code");
        const auto result = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        check(!result.isEmpty() && result.value("consistent").toBool() == (code == 0), "CLI emits truthful JSON");
    };
    run(args, 0);
    run(args + QStringList{"--unknown"}, 2);
    check(write(base_path, csv(partial)), "write incomplete CLI fixture");
    run(args, 1);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_five_minute_conservation();
    test_session_anchor_and_short_tail();
    test_gap_and_duplicate_refusal();
    test_volume_semantics_and_overflow();
    test_numeric_limits();
    test_reader_schema_and_bounds();
    test_directory_and_missing_sessions();
    test_cli_contract();
    std::printf("OHLCV consistency: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
