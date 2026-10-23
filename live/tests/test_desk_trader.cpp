// The Desk trader (live/desk_trader.hpp): reads every model and altair_trader's
// and the vol premium's files, counts only the sources that committed to a
// NIFTY direction, and paper-trades the majority at 10:30.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/desk_trader.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using namespace altair;
using namespace altair::live;
namespace fs = std::filesystem;

constexpr std::int64_t kSec = 1'000'000'000LL;

LiveInstrument inst(std::uint32_t tok, const char* sym, LiveKind k) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = std::string("NSE:") + sym; i.underlying = "NIFTY";
    i.kind = k; i.lot = 75; i.tick = 0.05; i.expiry_day = k == LiveKind::Index ? 0 : parse_day("2026-10-27");
    if (k == LiveKind::Call) i.strike = 25000.0;
    return i;
}

/// Opens one lot of `token` on `side` at 10:15, as a model that has committed.
class Stub final : public LiveModel {
public:
    Stub(std::string n, std::uint32_t token, int side, std::string fam = "stub")
        : n_(std::move(n)), fam_(std::move(fam)), token_(token), side_(side) {}
    std::string name() const override { return n_; }
    std::string family() const override { return fam_; }
    void on_minute(LiveEngine& e, int m) override {
        if (m != 10 * 60 + 15 || side_ == 0) return;
        if (const LiveInstrument* i = e.instrument(token_)) (void)e.book().open(n_, *i, side_, 1, e.clock_ns(), "stub", false);
    }
    LiveModelView view(const LiveEngine&) const override { LiveModelView v; v.name = n_; v.family = fam_; v.state = "watching"; return v; }

private:
    std::string n_, fam_;
    std::uint32_t token_;
    int side_;
};

void write(const fs::path& p, const std::string& text) { std::ofstream(p) << text; }

/// One session to 10:35 with the given stubs; returns the engine's Desk trader position side (0 if flat).
int session(const std::vector<std::pair<std::string, int>>& fut_votes, bool option_holder, const DeskTraderCall& call,
            std::string* note = nullptr, bool pairs_leg = false) {
    const std::vector<LiveInstrument> u{inst(kLiveNiftyToken, "NIFTY", LiveKind::Index), inst(2, "NIFTY26OCTFUT", LiveKind::Future),
                                        inst(3, "NIFTY26OCT25000CE", LiveKind::Call)};
    LiveExecPolicy instant;
    instant.latency_ns = 0;
    LiveEngine e(u, nullptr, instant);
    for (const auto& [n, side] : fut_votes) e.add_model(std::make_unique<Stub>(n, 2, side));
    if (option_holder) e.add_model(std::make_unique<Stub>("Option seller", 3, -1));
    if (pairs_leg) e.add_model(std::make_unique<Stub>("Pairs", 2, 1, "pairs"));
    auto owned = std::make_unique<DeskTraderModel>(call, DeskVolReading{});
    DeskTraderModel* desk = owned.get();
    e.add_model(std::move(owned));
    const std::int64_t day = parse_day("2026-10-05");
    const std::int64_t open_ns = (day * 86400 + 9 * 3600 + 15 * 60 - 19800) * kSec;
    // A print a minute and quotes every 5 s, so a quote is fresh at every minute's decisions.
    for (int k = 0; k <= 80 * 12; ++k) {
        const std::int64_t at = open_ns + k * 5 * kSec + 500'000'000;
        for (const std::uint32_t tok : {kLiveNiftyToken, 2u, 3u}) {
            const std::int64_t px = tok == 3u ? 15000 : 2500000;
            if (k % 12 == 0) {
                PricePayload p;
                p.token = tok; p.last_paise = px; p.last_qty = 75; p.exchange_ts_ns = at;
                e.on_trade(p, at);
            }
            QuotePayload q;
            q.token = tok; q.flags = kQuoteHasTop; q.bid = px - 5; q.ask = px + 5; q.bid_qty = 750; q.ask_qty = 750;
            e.on_quote(q, at + 1);
        }
    }
    if (note) *note = desk->view(e).reason;
    if (std::getenv("DESK_DEBUG")) { std::printf("    note: %s\n", desk->view(e).reason.c_str()); for (const auto& f : desk->view(e).fields) std::printf("      %s = %s\n", f.first.c_str(), f.second.c_str()); }
    const LivePosition* p = e.book().position("Desk trader", 2);
    return p == nullptr ? 0 : p->side;
}

} // namespace

int main() {
    std::printf("Desk trader\n");

    // ---- reading altair_trader's and the vol premium's files ----------------------
    const fs::path dir = fs::temp_directory_path() / "altair_desk_test";
    fs::create_directories(dir);
    write(dir / "days.csv",
          "date,index,model,expected_bp,move_bp,cost_bp,position,pnl,cum_gross\n"
          "2026-10-01,NIFTY,\"Ridge stack\",4.0,1.0,3.5,0,0,0\n"
          "2026-10-02,NIFTY,\"Gokhale stack (linear)\",-9.5,0,3.5,0,0,0\n"
          "2026-10-02,BANKNIFTY,\"CNN\",20.0,0,3.5,0,0,0\n");
    write(dir / "trades.csv",
          "date,index,side,net,arm,model\n2026-09-01,NIFTY,long,1000.5,daily,x\n2026-09-02,NIFTY,short,-400,daily,x\n"
          "2026-09-02,NIFTY,short,-99999,intraday,x\n2026-09-03,BANKNIFTY,short,77,daily,x\n");
    const auto call = desk_read_trader(dir);
    check(call.ok && call.model == "Gokhale stack (linear)" && call.expected_bp == -9.5 && call.cost_bp == 3.5
              && call.day == parse_day("2026-10-02"),
          "altair_trader: the last NIFTY daily row is the call, quoted model names and all");
    check(call.trips == 2 && std::fabs(call.net - 600.5) < 1e-9, "its record: NIFTY daily trips only");
    write(dir / "signals.csv", "instrument,date,expiry,sessions,implied_vol,har_vol,spread,realised_vol_after\n"
                               "NIFTY,2026-09-30,2026-10-27,18,13.10,11.90,1.20,\nBANKNIFTY,2026-10-01,2026-10-27,18,15,14,1,\n");
    const auto vol = desk_read_vol(dir / "signals.csv");
    check(vol.ok && vol.day == parse_day("2026-09-30") && std::fabs(vol.spread - 1.2) < 1e-12, "vol premium: the last NIFTY reading");
    check(!desk_read_trader(dir / "missing").ok && !desk_read_vol(dir / "missing.csv").ok, "missing files: no call, with the reason");
    fs::remove_all(dir);

    // ---- deciding --------------------------------------------------------------
    DeskTraderCall none;
    none.why = "absent";
    std::string note;
    check(session({{"Direction A", 1}, {"Direction B", 1}, {"Direction C", -1}}, true, none, &note) == 1,
          "two of three committed models long: the desk buys one NIFTY future lot at 10:30");
    check(note.find("2 up, 1 down") != std::string::npos, "and its card says how the votes fell");
    check(session({{"Direction A", 1}, {"Direction B", -1}}, false, none, &note) == 0 && note.find("tie") != std::string::npos,
          "a tie: no trade, and it says so");
    check(session({}, true, none, &note) == 0 && note.find("no source committed") != std::string::npos,
          "only an option seller: not a view on NIFTY's direction, so no vote and no trade");
    check(session({}, false, none, &note, true) == 0, "a pair's long NIFTY leg is a hedge: not a vote");

    DeskTraderCall fresh = call;   // made at 2026-10-02's close, for 2026-10-05 (a Monday)
    check(session({{"Direction A", 1}}, false, fresh) == 0, "altair_trader's DOWN call, beating its cost, ties one UP vote");
    check(session({}, false, fresh) == -1, "and alone it sells");
    DeskTraderCall stale = call;
    stale.day = parse_day("2026-09-23");
    check(session({}, false, stale, &note) == 0, "a call from days ago is not this session's: not counted");
    DeskTraderCall small = call;
    small.expected_bp = -2.0;
    check(session({}, false, small) == 0, "a call that does not beat its cost is not counted");

    std::printf("Desk trader: %d problem(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
