// CX02-B5 / B5b acceptance tests for desktop/kill_switch.hpp.
//
// Finding C17-014. A halt request file that was malformed or unreadable was
// shown as "No halt requested" -- the fail-safe direction displayed as safe --
// and the write that records a request was never checked, so a failed write
// left an empty file where a valid request had been.
//
// Tests 4-6 are CX02-B5b, from the independent review:
//   R-AB-038  the CLEAR direction asked for no reason at all
//   R-AB-039  the two-second poll erased the "halt was NOT recorded" notice
//   R-AB-045  a directory that cannot be read made "absent" read as safe
//
// Only the pure half is tested here: reading the file, writing it, recording a
// clear, and the text the panel shows for each state. The dialogs need a
// person; the predicates they use do not.
//
// No check description here may contain the substring FAIL.

#include "../kill_switch.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace altair::ui;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    std::fflush(stdout);
    if (!ok) { ++failures; }
}

void spit(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { f.write(bytes); }
}

QJsonArray read_json_array(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { return {}; }
    return QJsonDocument::fromJson(f.readAll()).array();
}

void clear_is_recorded_before_request_removal(const QString& path)
{
    std::printf("\n4b clear_is_recorded_before_request_removal\n");
    const KillRequest before = read_kill_request(path);
    const ClearRequestResult result = clear_kill_request(
        path, before, QStringLiteral("admin"),
        QStringLiteral("the feed came back"));
    check(result == ClearRequestResult::Cleared,
          "the clear operation records the audit before completing");
    check(!QFileInfo::exists(path),
          "the request file is removed only after its clear was recorded");

    const QJsonArray first_history =
        read_json_array(path + QStringLiteral(".cleared.json"));
    const QJsonObject rec = first_history.at(0).toObject();
    check(first_history.size() == 1,
          "the first clear creates one append-only audit entry");
    check(rec.value(QStringLiteral("cleared_by")).toString()
                  == QStringLiteral("admin")
              && rec.value(QStringLiteral("reason")).toString()
                     == QStringLiteral("the feed came back"),
          "the audit record retains who cleared it and why");
    check(rec.value(QStringLiteral("was_requested_by")).toString()
                  == QStringLiteral("smit")
              && rec.value(QStringLiteral("was_reason")).toString()
                     == QStringLiteral("drill"),
          "the record also retains who requested the halt and why");

    QJsonObject next;
    next[QStringLiteral("requested_at")] =
        QStringLiteral("2026-09-16T02:00:00Z");
    next[QStringLiteral("requested_by")] = QStringLiteral("second operator");
    next[QStringLiteral("reason")] = QStringLiteral("second drill");
    check(write_kill_request(path, next),
          "a later halt request can be written after the first clear");
    const ClearRequestResult second = clear_kill_request(
        path, read_kill_request(path), QStringLiteral("second admin"),
        QStringLiteral("second clear reason"));
    const QJsonArray history =
        read_json_array(path + QStringLiteral(".cleared.json"));
    check(second == ClearRequestResult::Cleared && history.size() == 2,
          "a second clear appends instead of replacing the first audit record");
    check(history.at(0).toObject()
                  .value(QStringLiteral("was_requested_by")).toString()
                  == QStringLiteral("smit")
              && history.at(1).toObject()
                     .value(QStringLiteral("was_requested_by")).toString()
                     == QStringLiteral("second operator"),
          "both the old and new clear history remain attributable");
}

struct InterleaveState {
    QString path;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread requester;
    bool probe_done = false;
    bool probe_acquired = false;
    bool request_done = false;
    bool request_written = false;
    bool replacement_written = false;
};

void interleave_request_at_audit_boundary(void* opaque)
{
    auto& state = *static_cast<InterleaveState*>(opaque);
    state.requester = std::thread([&state] {
        // Probe the exact stable lock file as an independent request actor.
        // The request then enters the normal writer and blocks behind clear.
        QLockFile probe(kill_request_lock_path(state.path));
        const bool acquired = probe.tryLock(0);
        if (acquired) { probe.unlock(); }
        {
            std::lock_guard guard(state.mutex);
            state.probe_acquired = acquired;
            state.probe_done = true;
        }
        state.changed.notify_all();

        QJsonObject request;
        request[QStringLiteral("requested_at")] =
            QStringLiteral("2026-09-16T03:00:00Z");
        request[QStringLiteral("requested_by")] = QStringLiteral("request actor");
        request[QStringLiteral("reason")] = QStringLiteral("new halt during clear");
        const bool written = write_kill_request(state.path, request);
        {
            std::lock_guard guard(state.mutex);
            state.request_written = written;
            state.request_done = true;
        }
        state.changed.notify_all();
    });

    bool probe_acquired = false;
    {
        std::unique_lock guard(state.mutex);
        const bool observed = state.changed.wait_for(
            guard, std::chrono::seconds(10), [&state] { return state.probe_done; });
        if (!observed) {
            return;
        }
        probe_acquired = state.probe_acquired;
        if (probe_acquired) {
            // On a broken implementation without a shared lock, let the
            // competing request commit before clear resumes. This makes the
            // lost-request regression deterministic rather than scheduler-led.
            (void)state.changed.wait_for(
                guard, std::chrono::seconds(10), [&state] { return state.request_done; });
        }
    }

    // Simulate a legacy/out-of-process writer which ignores the advisory lock.
    // Clear must compare the just-audited bytes again before removing the path.
    QJsonObject newer;
    newer[QStringLiteral("requested_at")] =
        QStringLiteral("2026-09-16T03:30:00Z");
    newer[QStringLiteral("requested_by")] = QStringLiteral("external newer request");
    newer[QStringLiteral("reason")] = QStringLiteral("do not delete this halt");
    newer[QStringLiteral("request_id")] = QStringLiteral("external-newer-id");
    QSaveFile replacement(state.path);
    if (!replacement.open(QIODevice::WriteOnly)) {
        return;
    }
    const QByteArray bytes = QJsonDocument(newer).toJson(QJsonDocument::Indented);
    if (replacement.write(bytes) != bytes.size()) {
        replacement.cancelWriting();
        return;
    }
    const bool written = replacement.commit();
    {
        std::lock_guard guard(state.mutex);
        state.replacement_written = written;
        // Keep the probe result available for the assertions after clear.
        state.probe_acquired = probe_acquired;
    }
}

void request_clear_interleaving_is_safe(const QString& path)
{
    std::printf("\n4d request_clear_interleaving_is_safe\n");
    QJsonObject original;
    original[QStringLiteral("requested_at")] =
        QStringLiteral("2026-09-16T02:30:00Z");
    original[QStringLiteral("requested_by")] = QStringLiteral("original operator");
    original[QStringLiteral("reason")] = QStringLiteral("original halt");
    check(write_kill_request(path, original),
          "the original halt is in place before the interleaving");

    InterleaveState state;
    state.path = path;
    const ClearRequestResult result = clear_kill_request(
        path, read_kill_request(path), QStringLiteral("admin"),
        QStringLiteral("reviewing the original halt"),
        &interleave_request_at_audit_boundary, &state);
    if (state.requester.joinable()) { state.requester.join(); }

    check(state.probe_done && !state.probe_acquired,
          "the request actor observes the clear actor's stable interprocess lock");
    check(state.replacement_written,
          "a newer external request is deterministically placed after audit commit");
    check(result == ClearRequestResult::RequestChanged,
          "clear refuses removal when request identity/content changes after audit");
    check(state.request_written
              && read_kill_request(path).state == HaltFileState::Present
              && read_kill_request(path).requested_by == QStringLiteral("request actor"),
          "the serialized request actor writes a fresh halt after clear releases the lock");
}

void clear_audit_failure_keeps_request(const QString& base_path)
{
    std::printf("\n4c clear_audit_failure_keeps_request\n");
    const QString path = base_path + QStringLiteral(".blocked");
    QJsonObject request;
    request[QStringLiteral("requested_by")] = QStringLiteral("operator");
    request[QStringLiteral("reason")] = QStringLiteral("test halt");
    check(write_kill_request(path, request),
          "a request is available before testing an audit failure");

    // A directory at the audit filename makes QSaveFile::open fail on every
    // supported platform without changing permissions on a real directory.
    const QString audit_path = path + QStringLiteral(".cleared.json");
    check(QDir().mkpath(audit_path),
          "the audit destination is deliberately unavailable as a file");
    const ClearRequestResult result = clear_kill_request(
        path, read_kill_request(path), QStringLiteral("admin"),
        QStringLiteral("test clear"));
    check(result == ClearRequestResult::RecordFailed,
          "a failed audit write refuses the clear");
    check(read_kill_request(path).state == HaltFileState::Present,
          "the halt request remains when its clear cannot be recorded");
    QDir(audit_path).removeRecursively();
    QFile::remove(path);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("CX02-B5/B5b -- the halt request file\n");

    const QString dir = QDir::current().absoluteFilePath(
        QStringLiteral("cx02_halt_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/kill_request.json");

    // ── 1 ────────────────────────────────────────────────────────────────
    std::printf("\n1 absent_present_and_unreadable_are_three_states\n");
    QFile::remove(path);
    const KillRequest none = read_kill_request(path);
    check(none.state == HaltFileState::Absent && !none.halted(),
          "no file, in a directory we can read, is Absent and not halted");

    QJsonObject o;
    o[QStringLiteral("requested_at")] = QStringLiteral("2026-09-16T01:00:00Z");
    o[QStringLiteral("requested_by")] = QStringLiteral("smit");
    o[QStringLiteral("reason")] = QStringLiteral("drill");
    check(write_kill_request(path, o), "a request is written");
    const KillRequest yes = read_kill_request(path);
    check(yes.state == HaltFileState::Present && yes.halted()
              && yes.requested_by == QStringLiteral("smit")
              && yes.reason == QStringLiteral("drill"),
          "and reads back as Present, with who and why");

    // ── 2 ────────────────────────────────────────────────────────────────
    std::printf("\n2 malformed_request_reads_unknown_not_absent\n");
    spit(path, "{");
    const KillRequest torn = read_kill_request(path);
    check(torn.state == HaltFileState::Unreadable,
          "a file holding \"{\" is Unreadable -- it used to read as no request");
    check(torn.halted(),
          "and Unreadable is treated as HALTED: a request that cannot be read"
          " may still be a request");
    const QString txt = halt_state_text(torn);
    check(txt.contains(QStringLiteral("UNKNOWN"))
              && !txt.contains(QStringLiteral("No halt requested")),
          "the panel says UNKNOWN, never \"No halt requested\"");

    spit(path, "[1,2,3]");
    check(read_kill_request(path).state == HaltFileState::Unreadable,
          "valid JSON that is not an object is Unreadable too");
    spit(path, "");
    check(read_kill_request(path).state == HaltFileState::Unreadable,
          "and so is an EMPTY file -- what a failed truncating write used to"
          " leave behind");

    const QString absent_txt = halt_state_text(none);
    check(absent_txt.contains(QStringLiteral("No halt requested")),
          "while a genuinely absent file still says no halt was requested");

    // ── 3 ────────────────────────────────────────────────────────────────
    std::printf("\n3 write_is_checked_and_atomic\n");
    check(write_kill_request(path, o), "a valid request is written back");
    const QString as_dir = dir + QStringLiteral("/is_a_directory");
    QDir().mkpath(as_dir);
    check(!write_kill_request(as_dir, o),
          "a write that cannot be committed reports failure -- the old code"
          " ignored the result of write()");
    check(read_kill_request(path).state == HaltFileState::Present,
          "and a failed write elsewhere left the existing request intact");

    // ── 4 ────────────────────────────────────────────────────────────────
    // R-AB-038. The dangerous direction is the one that must ask.
    std::printf("\n4 clear_refuses_without_a_reason\n");
    check(!confirm_enabled(true, true, QString()),
          "the CLEAR direction, phrase typed and no reason, stays disabled");
    check(!confirm_enabled(true, true, QStringLiteral("   ")),
          "whitespace is not a reason");
    check(confirm_enabled(true, true, QStringLiteral("circuit breaker lifted")),
          "with a reason it is enabled");
    check(!confirm_enabled(false, true, QStringLiteral("a reason")),
          "and never without the exact phrase");
    check(confirm_enabled(true, false, QString()),
          "the REQUEST direction needs no reason -- it fails safe");

    clear_is_recorded_before_request_removal(path);
    request_clear_interleaving_is_safe(path);
    clear_audit_failure_keeps_request(path);

    // ── 5 ────────────────────────────────────────────────────────────────
    // R-AB-039. The message that says a halt was NOT recorded must survive.
    std::printf("\n5 a_failed_write_notice_survives_the_poll\n");
    const QString notice = QStringLiteral("Could not write the halt request.");
    const QString panel = halt_panel_text(none, notice);
    check(panel.contains(notice),
          "a latched action failure is rendered by the refresh the poll calls");
    check(panel.contains(QStringLiteral("No halt requested")),
          "above the file's own state, not instead of it");
    check(halt_panel_text(none, QString()) == halt_state_text(none),
          "and with nothing latched the panel is exactly the file's state");

    // ── 6 ────────────────────────────────────────────────────────────────
    // R-AB-045. "Not there" and "cannot tell" are different answers.
    std::printf("\n6 an_unexaminable_path_is_unreadable_not_absent\n");
    const QString gone = dir + QStringLiteral("/no_such_dir/kill_request.json");
    const KillRequest g = read_kill_request(gone);
    check(g.state == HaltFileState::Unreadable && g.halted(),
          "a request file whose DIRECTORY does not exist is Unreadable, not"
          " Absent -- the fail-safe direction must not read as safe");
    check(read_kill_request(as_dir).state == HaltFileState::Unreadable,
          "and a path that exists but cannot be opened as a file is Unreadable");

    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".cleared.json"));
    QDir(dir).removeRecursively();
    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
