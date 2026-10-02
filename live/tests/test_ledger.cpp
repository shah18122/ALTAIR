// The paper ledger on disk when the disk misbehaves: rows are appended
// all-or-nothing, a write that fails part-way is cut back so the retry writes
// each row exactly once, the views never run ahead of the journal, and the
// engine halts new entries while anything is unwritten and resumes when it
// lands -- with a journal that rebuilds exactly the book the engine holds.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/engine.hpp>
#include <live/ledger.hpp>
#include <live/paper.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#endif

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair;
using namespace altair::live;
namespace fs = std::filesystem;

constexpr std::int64_t kSec = 1'000'000'000LL;
const std::int64_t kToday = parse_day("2026-10-01");

std::int64_t at(int hh, int mm, int ss = 0) { return (kToday * 86400 + hh * 3600 + mm * 60 + ss - 19800) * kSec; }

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
std::size_t lines(const fs::path& p) {
    const std::string s = read(p);
    std::size_t n = 0;
    for (const char c : s) n += c == '\n' ? 1u : 0u;
    return n;
}

LiveInstrument inst(std::uint32_t tok, const char* sym, LiveKind k, std::int64_t lot, std::int64_t expiry = 0) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = std::string("NSE:") + sym; i.underlying = "NIFTY"; i.kind = k;
    i.lot = lot; i.expiry_day = expiry; i.tick = 0.05;
    return i;
}
void quote(LiveEngine& e, std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t qty, std::int64_t ns) {
    QuotePayload q;
    q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = qty; q.ask_qty = qty;
    e.on_quote(q, ns);
}
void trade(LiveEngine& e, std::uint32_t tok, std::int64_t px, std::int64_t ns) {
    PricePayload p;
    p.token = tok; p.last_paise = px;
    e.on_trade(p, ns);
}
double cost5bp(const LiveInstrument&, bool, double q, double px, std::int64_t) { return 0.0005 * q * px; }

} // namespace

int main() {
    std::printf("live ledger\n");
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path()
                       / ("altair_ledger_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_"
                          + std::to_string(std::random_device{}()));
    fs::create_directories(dir, ec);

    // ---- one log: header once, rows once, in order --------------------------------
    {
        LiveCsvLog log(dir / "a.csv", "x,y");
        check(log.flush() && !fs::exists(dir / "a.csv"), "nothing queued: nothing written, not even a header");
        log.add("1,2");
        log.add("3,4");
        check(log.flush() && read(dir / "a.csv") == "x,y\n1,2\n3,4\n" && log.pending() == 0, "the header once, then the rows");
        log.add("5,6");
        check(log.flush() && read(dir / "a.csv") == "x,y\n1,2\n3,4\n5,6\n", "appended, never rewritten");
    }

    // ---- a path that cannot be opened: rows kept, file untouched ---------------------
    {
        LiveCsvLog log(dir / "gone" / "b.csv", "h");
        { std::ofstream(dir / "gone") << "a file where the directory should be"; }
        log.add("r1");
        check(!log.flush() && log.pending() == 1, "an unwritable path: the flush says so and keeps the row");
        log.add("r2");
        fs::remove(dir / "gone", ec);
        fs::create_directories(dir / "gone", ec);
        check(log.flush() && read(dir / "gone" / "b.csv") == "h\nr1\nr2\n" && log.pending() == 0,
              "the path comes back: both rows, once each, in order");
    }

    // ---- a torn tail from a failed attempt is cut before the retry ---------------------
    {
        LiveCsvLog log(dir / "c.csv", "h");
        log.add("good");
        (void)log.flush();
        { std::ofstream(dir / "c.csv", std::ios::app) << "half a ro"; }   // what a write cut off part-way leaves
        log.add("next");
        check(log.flush() && read(dir / "c.csv") == "h\ngood\nnext\n", "the torn half-row is cut; the retry follows the last good row");
    }

#if !defined(_WIN32)
    // ---- a write that fails part-way (the file-size limit: EFBIG, like a full disk) ----
    {
        LiveCsvLog log(dir / "d.csv", "time,what");
        log.add("1,first");
        (void)log.flush();
        const std::string before = read(dir / "d.csv");
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit old{};
        getrlimit(RLIMIT_FSIZE, &old);
        rlimit lim = old;
        lim.rlim_cur = static_cast<rlim_t>(before.size() + 40);   // room for part of what follows, not all
        const bool limited = setrlimit(RLIMIT_FSIZE, &lim) == 0;
        for (int i = 0; i < 20; ++i) log.add(std::to_string(i + 2) + ",a row long enough to cross the limit part-way");
        const bool refused = !log.flush();
        const std::string during = read(dir / "d.csv");
        setrlimit(RLIMIT_FSIZE, &old);
        check(limited && refused && log.pending() == 20, "the disk fills part-way through: the flush says so, every row kept");
        check(during == before, "and the file is cut back to its last good row: no torn tail left behind");
        check(log.flush() && lines(dir / "d.csv") == 22 && read(dir / "d.csv").rfind(before, 0) == 0,
              "space returns: all twenty land, once each, after the first");
    }
#endif

    // ---- the ledger with the engine: halt on a write that fails, resume when it lands --
    {
        const fs::path paper = dir / "paper";
        fs::create_directories(paper, ec);
        const std::vector<LiveInstrument> u{inst(kLiveNiftyToken, "NIFTY 50", LiveKind::Index, 1),
                                            inst(10, "FUT_A", LiveKind::Future, 50, kToday + 20),
                                            inst(11, "FUT_B", LiveKind::Future, 50, kToday + 20)};
        LiveEngine e(u, cost5bp, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        LivePaperLedger ledger(paper, "TEST");
        trade(e, kLiveNiftyToken, 2400000, at(10, 0));
        quote(e, 10, 2399900, 2400100, 500, at(10, 0));
        quote(e, 11, 2399900, 2400100, 500, at(10, 0));
        check(e.book().open("m", u[1], 1, 1, at(10, 0), "in", false), "an entry fills");
        (void)ledger.collect(e);
        check(ledger.flush(e).empty() && lines(paper / "journal.csv") == 2, "and reaches the journal");

        // The journal cannot be written (a directory where the file was).
        const std::string journal_before = read(paper / "journal.csv");
        fs::rename(paper / "journal.csv", dir / "journal.saved", ec);
        fs::create_directories(paper / "journal.csv", ec);
        trade(e, kLiveNiftyToken, 2400000, at(10, 1));
        quote(e, 10, 2400900, 2401100, 500, at(10, 1));
        quote(e, 11, 2399900, 2400100, 500, at(10, 1));
        check(e.book().close("m", 10, at(10, 1), "out") && e.book().position("m", 10) == nullptr, "an exit fills");
        (void)ledger.collect(e);
        const std::string failed = ledger.flush(e);
        check(failed.find("journal.csv") != std::string::npos, "the journal write fails, and says which file");
        check(!fs::exists(paper / "trades.csv"), "the views wait: no round trip shown that the record lacks");
        e.set_halt(LivePaperLedger::halt_text(failed));
        std::string why;
        check(!e.book().open("m", u[2], 1, 1, at(10, 1), "in", false, &why) && why.find("halted") != std::string::npos,
              "while it is unwritten, no new entry");
        check(ledger.pending() >= 2, "and the rows wait in memory");

        // The disk comes back.
        fs::remove_all(paper / "journal.csv", ec);
        fs::rename(dir / "journal.saved", paper / "journal.csv", ec);
        const std::string after = ledger.flush(e);
        e.set_halt(LivePaperLedger::halt_text(after));
        check(after.empty() && e.halt().empty() && ledger.pending() == 0, "the next flush lands everything and lifts the halt");
        check(read(paper / "journal.csv").rfind(journal_before, 0) == 0 && lines(paper / "journal.csv") == 3,
              "the journal: the entry once, then the exit once");
        check(lines(paper / "trades.csv") == 2 && lines(paper / "fills.csv") == 3, "and the views caught up");
        std::vector<std::string> orphans;
        std::size_t rows = 0;
        const auto rebuilt = live_replay_journal((paper / "journal.csv").string(), u, orphans, &rows);
        check(rows == 2 && rebuilt.empty() && e.book().positions().empty(), "the journal rebuilds the book the engine holds: flat");
        quote(e, 11, 2399900, 2400100, 500, at(10, 2));
        trade(e, kLiveNiftyToken, 2400000, at(10, 2));
        check(e.book().open("m", u[2], 1, 1, at(10, 2), "in", false, &why), "and entries are allowed again");
    }

    fs::remove_all(dir, ec);
    std::printf("%s\n", failures == 0 ? "all live ledger checks passed" : "live ledger checks did not pass");
    return failures == 0 ? 0 : 1;
}
