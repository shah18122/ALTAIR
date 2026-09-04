// app/tests/test_warm_restart.cpp -- P12-02.
//
// The four ways a warm restart resumes into a state that is wrong, each of
// which produces a file that PARSES:
//
//   1. torn by the crash it exists to survive
//   2. arithmetically broken but internally consistent
//   3. from yesterday
//   4. carrying a breach that a restart would clear
//
// Every one of them looks like a clean resume from the outside.

#include <app/warm_restart.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

std::string tmp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

/// A consistent snapshot: bought 100 units at Rs 250.00 with Rs 12.00 of cost,
/// out of Rs 1,00,000.00.
altair::Snapshot good() {
    altair::Snapshot s{};
    s.session_day = 20260904;
    s.tick_seqno = 12345;
    s.initial_cash_paise = 10'000'000;      // Rs 1,00,000.00
    s.gross_fills_paise = 2'500'000;        // 100 x Rs 250.00
    s.gross_costs_paise = 1'200;            // Rs 12.00
    s.cash_paise = s.initial_cash_paise - s.gross_fills_paise
                 - s.gross_costs_paise;
    s.position_units = 100;
    return s;
}

} // namespace

int main() {
    std::printf("P12-02 warm restart\n");

    const std::string path = tmp_path("altair_snap_test.txt");
    std::filesystem::remove(path);

    // ---- the happy path ---------------------------------------------------
    {
        const auto w = altair::save_snapshot(path.c_str(), good());
        check(w.has_value(), "a consistent snapshot writes");
        const auto r = altair::load_snapshot(path.c_str(), 20260904);
        check(r.has_value(), "and reads back");
        if (r) {
            check(r->tick_seqno == 12345 && r->position_units == 100
                      && r->cash_paise == good().cash_paise,
                  "every field survives the round trip exactly");
        }
    }

    // ---- 1. TORN BY THE CRASH IT EXISTS TO SURVIVE ------------------------
    //
    // A process dying mid-write leaves a file that is half old and half new,
    // and it parses. The checksum is what catches it.
    {
        std::string all;
        {
            std::ifstream f(path, std::ios::binary);
            all.assign((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        }
        // Flip one digit in the cash figure. Everything still parses; the
        // number is simply not the one that was written.
        const std::size_t at = all.find("cash ");
        std::string torn = all;
        torn[at + 5] = (torn[at + 5] == '9') ? '8' : '9';
        const std::string tp = tmp_path("altair_snap_torn.txt");
        { std::ofstream f(tp, std::ios::binary); f << torn; }

        const auto r = altair::load_snapshot(tp.c_str(), 20260904);
        check(!r && r.error() == altair::RestoreError::ChecksumMismatch,
              "a single flipped digit is caught by the checksum — the file "
              "still PARSES, which is why parsing is not the check");
        std::filesystem::remove(tp);
    }

    // ---- 2. ARITHMETICALLY BROKEN, INTERNALLY CONSISTENT ------------------
    //
    // A checksum proves the bytes survived. It says nothing about whether the
    // numbers were right when they were written, and a snapshot taken from an
    // engine that had already lost a paise carries that loss forward with a
    // perfectly valid checksum.
    {
        altair::Snapshot s = good();
        s.cash_paise += 1;                  // one paise that came from nowhere
        const auto w = altair::save_snapshot(path.c_str(), s);
        check(!w && w.error() == altair::RestoreError::InvariantBreach,
              "a snapshot that fails conservation REFUSES TO BE WRITTEN — "
              "persisting it makes the next restart look like a storage fault");

        // And if one reaches disk by another route, the load refuses too.
        const std::string bp = tmp_path("altair_snap_bad.txt");
        {
            // Written by hand WITH a correct checksum, so only the arithmetic
            // is wrong. This is the case a checksum cannot see.
            std::string body =
                "altair-snapshot 1\n"
                "session_day 20260904\ntick_seqno 1\ninitial_cash 10000000\n"
                "cash 7498801\nposition 100\ngross_fills 2500000\n"
                "gross_costs 1200\nbreached 0\n";
            std::ofstream f(bp, std::ios::binary);
            f << body << "checksum " << altair::detail::snapshot_hash(body)
              << '\n';
        }
        const auto r = altair::load_snapshot(bp.c_str(), 20260904);
        check(!r && r.error() == altair::RestoreError::InvariantBreach,
              "a VALID checksum over WRONG arithmetic is still refused — the "
              "two checks answer different questions");
        std::filesystem::remove(bp);
    }

    // ---- 3. FROM YESTERDAY ------------------------------------------------
    //
    // Resuming yesterday's positions is worse than starting flat: the engine
    // believes it holds something it does not and hedges against it.
    {
        const auto w = altair::save_snapshot(path.c_str(), good());
        check(w.has_value(), "re-wrote the good snapshot");
        const auto r = altair::load_snapshot(path.c_str(), 20260905);
        check(!r && r.error() == altair::RestoreError::StaleSession,
              "a snapshot from a DIFFERENT session refuses by default");
        const auto forced = altair::load_snapshot(path.c_str(), 0);
        check(forced.has_value(),
              "and session 0 is the explicit bypass — a parameter at the call "
              "site, not a default somebody inherits");
    }

    // ---- 4. A BREACH THAT A RESTART WOULD CLEAR ---------------------------
    //
    // The worst of the four. If the ledger tripped and the snapshot carries
    // that, resuming from it must NOT quietly un-trip the kill switch --
    // otherwise a reboot is a way to clear a halt.
    {
        altair::Snapshot s = good();
        s.breached = true;
        const auto w = altair::save_snapshot(path.c_str(), s);
        check(w.has_value(),
              "a breached snapshot still WRITES — the record of the breach is "
              "the thing worth keeping");
        const auto r = altair::load_snapshot(path.c_str(), 20260904);
        check(!r && r.error() == altair::RestoreError::BreachLatched,
              "but it refuses to RESUME: restarting must not be a way to "
              "clear a kill switch");
    }

    // ---- the write is atomic ----------------------------------------------
    {
        const auto w = altair::save_snapshot(path.c_str(), good());
        check(w.has_value(), "wrote");
        check(!std::filesystem::exists(path + ".tmp"),
              "and left no .tmp behind — the rename is what makes the file "
              "either the old snapshot or the whole new one, never a splice");
    }

    // ---- shape refusals ---------------------------------------------------
    {
        const std::string np = tmp_path("altair_snap_none.txt");
        std::filesystem::remove(np);
        const auto r = altair::load_snapshot(np.c_str(), 0);
        check(!r && r.error() == altair::RestoreError::NoSnapshot,
              "a missing snapshot is NoSnapshot, not an empty success");

        const std::string gp = tmp_path("altair_snap_garbage.txt");
        { std::ofstream f(gp); f << "hello\n"; }
        const auto g = altair::load_snapshot(gp.c_str(), 0);
        check(!g && g.error() == altair::RestoreError::Malformed,
              "and a file that is not a snapshot is refused, not parsed");
        std::filesystem::remove(gp);
    }

    std::filesystem::remove(path);
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
