// app/activity_log.hpp -- ALTAIR's one activity log: what ran on this PC, and when.
//
// Every unattended job (the feed recorder, the nightly store and retrain, the
// scheduled tasks in ops/) appends here, so a morning look at ONE file says
// what happened overnight:
//
//     data/logs/altair-YYYY-MM-DD.log        (IST day; git-ignored by logs/)
//
// One line an event:
//
//     2026-10-26T09:14:02.381+05:30 [recorder] INFO tape opened data/store/tapes/...
//
// SEVERAL WRITERS, ONE FILE. The C++ binaries and the PowerShell scripts both
// write it. On Windows the file is opened for append with writers denied
// (_SH_DENYWR), and ops/altair_log.ps1 opens it with FileShare.Read -- the same
// rule from both sides, so two writers wait for each other instead of
// interleaving. On POSIX a single O_APPEND write of one line is atomic.
// A writer that cannot get the file in about two seconds drops the line and
// says so on stdout: a log must never stall a recorder.
//
// NO SECRETS. Lines go through redact(): anything after a key that names a
// credential (access_token, refresh_token, secret, password, pin, authorization,
// Bearer) is replaced by "<redacted>". Callers still must not pass credentials;
// this is the second fence, not the first.
#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <share.h>
#  include <sys/stat.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace altair::activity {

inline constexpr int kIstOffsetSec = 5 * 3600 + 30 * 60;

/// "YYYY-MM-DD" of the IST day holding `unix_ms`.
[[nodiscard]] inline std::string ist_day(std::int64_t unix_ms) {
    const std::int64_t days = (unix_ms / 1000 + kIstOffsetSec) / 86400;
    // civil_from_days (Howard Hinnant), as elsewhere in app/: no host time zone.
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", static_cast<long long>(y + (m <= 2 ? 1 : 0)), m, d);
    return buf;
}

/// "YYYY-MM-DDTHH:MM:SS.mmm+05:30" for `unix_ms`.
[[nodiscard]] inline std::string ist_stamp(std::int64_t unix_ms) {
    const std::int64_t ist_ms = unix_ms + static_cast<std::int64_t>(kIstOffsetSec) * 1000;
    const std::int64_t sod = ((ist_ms / 1000) % 86400 + 86400) % 86400;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%sT%02lld:%02lld:%02lld.%03lld+05:30", ist_day(unix_ms).c_str(),
                  static_cast<long long>(sod / 3600), static_cast<long long>((sod / 60) % 60),
                  static_cast<long long>(sod % 60), static_cast<long long>(((ist_ms % 1000) + 1000) % 1000));
    return buf;
}

[[nodiscard]] inline std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// The line with every credential-looking value masked, and no newlines.
[[nodiscard]] inline std::string redact(std::string_view in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) s.push_back(c == '\n' || c == '\r' ? ' ' : c);
    std::string lower = s;
    for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    static constexpr std::string_view keys[] = {"bearer", "access_token", "refresh_token", "secret", "password",
                                                "pin", "authorization", "auth_code", "api_key"};
    for (const auto key : keys) {
        for (std::size_t at = lower.find(key); at != std::string::npos; at = lower.find(key, at + 1)) {
            // A whole word only: "pin" inside "spinning" or "pinned" is not a key.
            const bool left = at == 0 || !std::isalnum(static_cast<unsigned char>(lower[at - 1]));
            const std::size_t end = at + key.size();
            const bool right = end >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[end]));
            if (!left || !right) continue;
            std::size_t v = end;
            while (v < s.size() && (s[v] == ' ' || s[v] == '"' || s[v] == ':' || s[v] == '=')) ++v;
            std::size_t e = v;
            while (e < s.size() && s[e] != ' ' && s[e] != ',' && s[e] != '"' && s[e] != '}') ++e;
            if (e == v) continue;
            s.replace(v, e - v, "<redacted>");
            lower.replace(v, e - v, "<redacted>");
            at = v;
        }
    }
    return s;
}

/// Append one line. `root` is the ALTAIR tree (data/logs/ is made under it).
/// False when the line could not be written; the caller carries on.
inline bool write(const std::filesystem::path& root, std::string_view component,
                  std::string_view level, std::string_view message) {
    const std::int64_t ms = now_ms();
    const std::filesystem::path dir = root / "data" / "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / ("altair-" + ist_day(ms) + ".log");
    std::string line = ist_stamp(ms);
    line += " [";
    line += component;
    line += "] ";
    line += level;
    line += ' ';
    line += redact(message);
    line += '\n';
#if defined(_WIN32)
    int fd = -1;
    for (int i = 0; i < 80 && fd < 0; ++i) {
        const errno_t rc = _wsopen_s(&fd, file.c_str(), _O_CREAT | _O_WRONLY | _O_APPEND | _O_BINARY,
                                     _SH_DENYWR, _S_IREAD | _S_IWRITE);
        if (rc == 0 && fd >= 0) break;
        fd = -1;
        if (rc != EACCES) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (fd < 0) { std::printf("activity log busy; line dropped: %s", line.c_str()); return false; }
    const bool ok = _write(fd, line.data(), static_cast<unsigned>(line.size())) == static_cast<int>(line.size());
    (void)_close(fd);
#else
    const int fd = ::open(file.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) { std::printf("activity log unavailable; line dropped: %s", line.c_str()); return false; }
    const bool ok = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    (void)::close(fd);
#endif
    return ok;
}

/// Remove altair-YYYY-MM-DD.log files older than `keep_days` IST days.
inline std::size_t prune(const std::filesystem::path& root, int keep_days) {
    const std::filesystem::path dir = root / "data" / "logs";
    const std::string oldest = ist_day(now_ms() - static_cast<std::int64_t>(keep_days) * 86'400'000LL);
    std::size_t removed = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() != 21 || n.rfind("altair-", 0) != 0 || e.path().extension() != ".log") continue;
        if (n.substr(7, 10) < oldest) removed += std::filesystem::remove(e.path(), ec) ? 1u : 0u;
    }
    return removed;
}

} // namespace altair::activity
