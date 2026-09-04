// app/warm_restart.hpp -- crash recovery and warm restart from state.
//
// P12-02.
//
// A RESTART THAT CANNOT PROVE ITS STATE MUST REFUSE TO RESUME.
//
// This is the card. The obvious warm restart writes the ledger and the
// positions to a file, reads them back, and carries on. Every step of that is
// easy and the failure is silent: a snapshot torn by the crash it exists to
// survive reloads as a plausible position, the engine resumes against a market
// it thinks it is flat in, and nothing anywhere reports a problem until a
// broker reconciliation days later.
//
// So the snapshot carries the CONSERVATION INVARIANT with it, and the restore
// recomputes it. CLAUDE.md: "Sigma(fills) + Sigma(costs) + cash_delta == 0
// exactly, in paise, checked every tick. A breach trips the kill switch."
// A snapshot whose numbers no longer satisfy that is not a state to resume
// from -- it is evidence of corruption, and it is treated as such.
//
// THE CRASH HAPPENS DURING THE WRITE. THAT IS THE WHOLE PROBLEM.
//
// A process that dies mid-fwrite leaves a file that is half old and half new,
// and it parses. So:
//
//   * the snapshot is written to a TEMP and RENAMED over the target. Rename is
//     atomic on NTFS and POSIX, so the file is either the previous snapshot or
//     the whole new one, never a splice of the two.
//   * it carries a CHECKSUM over its own body, so a torn write from any other
//     cause is caught rather than parsed.
//   * and it carries the invariant, so a snapshot that is internally consistent
//     but ARITHMETICALLY wrong is caught too. A checksum proves the bytes
//     survived; it says nothing about whether the numbers were right when they
//     were written.
//
// AND A STALE SNAPSHOT IS NOT A CRASH RECOVERY.
//
// Resuming from yesterday's positions is worse than starting flat, because the
// engine will believe it holds something it does not and hedge against it. The
// snapshot carries the session date and the tick seqno it was taken at, and a
// restore into a DIFFERENT session refuses by default -- `--resume-stale` is
// an explicit operator decision, not a fallback.

#pragma once

#include <core/invariant/conservation.hpp>
#include <core/types/units.hpp>

#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace altair {

enum class RestoreError : std::uint8_t {
    NoSnapshot,
    /// The file exists but is not the shape a snapshot has.
    Malformed,
    /// The stored checksum does not match the body. A torn or edited file.
    ChecksumMismatch,
    /// The numbers do not satisfy conservation. Internally consistent bytes
    /// carrying arithmetic that was already wrong when written.
    InvariantBreach,
    /// From a different session. Not an error the caller may ignore silently.
    StaleSession,
    /// The snapshot records a breach that had already tripped. Resuming would
    /// clear a kill switch by restarting, which is the one thing a restart
    /// must never do.
    BreachLatched
};

[[nodiscard]] inline const char* restore_error_text(RestoreError e) noexcept {
    switch (e) {
    case RestoreError::NoSnapshot:       return "no snapshot file";
    case RestoreError::Malformed:        return "not a snapshot";
    case RestoreError::ChecksumMismatch: return "checksum mismatch — torn or edited";
    case RestoreError::InvariantBreach:  return "conservation does not hold in the snapshot";
    case RestoreError::StaleSession:     return "snapshot is from a different session";
    case RestoreError::BreachLatched:    return "the snapshot records a LATCHED BREACH";
    }
    return "unknown";
}

/// Everything a warm restart needs, and nothing it does not.
///
/// Deliberately flat and small. A snapshot that tries to carry the whole
/// engine is a snapshot whose format changes every card, and a format that
/// changes is one that silently fails to load an older file.
struct Snapshot {
    /// Session this was taken in, as YYYYMMDD. A restore into a different one
    /// refuses -- see the header.
    std::int64_t session_day = 0;
    /// The replayer/feed position, so a resume knows where it was.
    std::uint64_t tick_seqno = 0;

    std::int64_t initial_cash_paise = 0;
    std::int64_t cash_paise = 0;
    std::int64_t position_units = 0;
    std::int64_t gross_fills_paise = 0;
    std::int64_t gross_costs_paise = 0;
    /// True when the ledger had already tripped. Carried so a restart cannot
    /// be used to clear it.
    bool breached = false;
};

namespace detail {

/// FNV-1a over the snapshot's own text body.
///
/// Detects CHANGE, not tampering: a crash-torn file and a hand-edited one both
/// fail, which is what is wanted. If this ever needs to resist an adversary it
/// is a different field with a different name.
[[nodiscard]] inline std::uint64_t snapshot_hash(const std::string& body) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : body) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

[[nodiscard]] inline std::string snapshot_body(const Snapshot& s) {
    std::ostringstream o;
    o << "altair-snapshot 1\n"
      << "session_day " << s.session_day << '\n'
      << "tick_seqno " << s.tick_seqno << '\n'
      << "initial_cash " << s.initial_cash_paise << '\n'
      << "cash " << s.cash_paise << '\n'
      << "position " << s.position_units << '\n'
      << "gross_fills " << s.gross_fills_paise << '\n'
      << "gross_costs " << s.gross_costs_paise << '\n'
      << "breached " << (s.breached ? 1 : 0) << '\n';
    return o.str();
}

} // namespace detail

/// Does the snapshot's own arithmetic hold?
///
/// cash must equal initial_cash minus what went out. Checked on WRITE as well
/// as on read: writing a snapshot that already fails is how a corrupt state
/// gets persisted in the first place, and catching it at the write names the
/// tick that produced it rather than the restart that found it.
[[nodiscard]] inline bool conservation_holds(const Snapshot& s) noexcept {
    // The ledger's own identity: initial_cash - cash == net outflow, and net
    // outflow is fills plus costs. Integer paise throughout -- this is exactly
    // the comparison that must never be done in double.
    const std::int64_t spent = s.initial_cash_paise - s.cash_paise;
    return spent == s.gross_fills_paise + s.gross_costs_paise;
}

/// Write atomically: temp, then rename.
[[nodiscard]] inline std::expected<void, RestoreError>
save_snapshot(const char* path, const Snapshot& s) {
    // REFUSE TO PERSIST A BROKEN STATE. A snapshot that fails conservation is
    // not worth keeping and writing it makes the next restart's failure look
    // like a storage problem rather than an engine one.
    if (!conservation_holds(s)) {
        return std::unexpected(RestoreError::InvariantBreach);
    }
    const std::string body = detail::snapshot_body(s);
    const std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { return std::unexpected(RestoreError::NoSnapshot); }
        f << body << "checksum " << detail::snapshot_hash(body) << '\n';
        if (!f) { return std::unexpected(RestoreError::NoSnapshot); }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) { return std::unexpected(RestoreError::NoSnapshot); }
    return {};
}

/// Read, verify, and refuse rather than guess.
///
/// `current_session_day` is the session being resumed INTO. Pass 0 to skip the
/// staleness check, which is what `--resume-stale` does -- and it is a
/// parameter rather than a default so the bypass is visible at the call site.
[[nodiscard]] inline std::expected<Snapshot, RestoreError>
load_snapshot(const char* path, std::int64_t current_session_day) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return std::unexpected(RestoreError::NoSnapshot); }
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (all.rfind("altair-snapshot 1\n", 0) != 0) {
        return std::unexpected(RestoreError::Malformed);
    }
    const std::size_t ck = all.rfind("\nchecksum ");
    if (ck == std::string::npos) {
        return std::unexpected(RestoreError::Malformed);
    }
    const std::string body = all.substr(0, ck + 1);
    // Parsed with a stream, not sscanf: MSVC deprecates the latter and gate 1
    // is zero warnings, and suppressing the check with _CRT_SECURE_NO_WARNINGS
    // would silence it everywhere else too.
    std::uint64_t stored = 0;
    {
        std::istringstream cs(all.substr(ck + 1));
        std::string tag;
        if (!(cs >> tag >> stored) || tag != "checksum") {
            return std::unexpected(RestoreError::Malformed);
        }
    }
    if (stored != detail::snapshot_hash(body)) {
        return std::unexpected(RestoreError::ChecksumMismatch);
    }

    Snapshot s{};
    std::istringstream in(body);
    std::string line, key;
    std::getline(in, line);                       // the magic
    int seen = 0;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        ls >> key;
        long long v = 0;
        if (!(ls >> v)) { continue; }
        if (key == "session_day")       { s.session_day = v; ++seen; }
        else if (key == "tick_seqno")   { s.tick_seqno = static_cast<std::uint64_t>(v); ++seen; }
        else if (key == "initial_cash") { s.initial_cash_paise = v; ++seen; }
        else if (key == "cash")         { s.cash_paise = v; ++seen; }
        else if (key == "position")     { s.position_units = v; ++seen; }
        else if (key == "gross_fills")  { s.gross_fills_paise = v; ++seen; }
        else if (key == "gross_costs")  { s.gross_costs_paise = v; ++seen; }
        else if (key == "breached")     { s.breached = v != 0; ++seen; }
    }
    if (seen != 8) { return std::unexpected(RestoreError::Malformed); }

    // ORDER MATTERS BELOW, and it is the order of how bad each answer is.
    //
    // A latched breach is checked FIRST. If the ledger had tripped, the one
    // thing a restart must not do is clear it by starting again -- that turns
    // the kill switch into a reboot away from nothing.
    if (s.breached) { return std::unexpected(RestoreError::BreachLatched); }
    if (!conservation_holds(s)) {
        return std::unexpected(RestoreError::InvariantBreach);
    }
    if (current_session_day != 0 && s.session_day != current_session_day) {
        return std::unexpected(RestoreError::StaleSession);
    }
    return s;
}

} // namespace altair
