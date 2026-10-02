// Tests for live/universe.hpp: which contracts the live feed streams, and
// the Kite-token to FYERS-ticker pairing the whole terminal depends on.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/universe.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

const altair::live::LiveInstrument* find(const altair::live::LiveUniverse& u, const std::string& sym) {
    for (const auto& i : u.instruments)
        if (i.symbol == sym) return &i;
    return nullptr;
}

}  // namespace

int main() {
    using namespace altair::live;
    std::printf("live universe\n");
    const auto dir = std::filesystem::temp_directory_path() / "altair_live_universe_test";
    std::filesystem::create_directories(dir);
    const std::string master = (dir / "instruments.csv").string();
    {
        std::ofstream f(master);
        f << "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,lot_size,"
             "instrument_type,segment,exchange\n";
        f << "256265,1001,NIFTY 50,\"NIFTY 50\",0,,0,0,0,EQ,INDICES,NSE\n";
        f << "17512194,68407,NIFTY26SEPFUT,\"NIFTY\",0,2026-09-29,0,0.1,65,FUT,NFO-FUT,NFO\n";   // expired
        f << "12468226,48704,NIFTY26OCTFUT,\"NIFTY\",0,2026-10-27,0,0.1,65,FUT,NFO-FUT,NFO\n";
        f << "15736578,61471,NIFTY26NOVFUT,\"NIFTY\",0,2026-11-23,0,0.1,65,FUT,NFO-FUT,NFO\n";
        f << "15736579,61472,NIFTY26DECFUT,\"NIFTY\",0,2026-12-29,0,0.1,65,FUT,NFO-FUT,NFO\n";
        // Weekly 2026-10-06: strikes 23700..24300 step 100; a later monthly that must not be chosen.
        unsigned tok = 10000000;
        for (int k = 23700; k <= 24300; k += 100) {
            for (const char* t : {"CE", "PE"}) {
                f << ++tok << ",1,NIFTY26O06" << k << t << ",\"NIFTY\",0,2026-10-06," << k << ",0.05,65," << t
                  << ",NFO-OPT,NFO\n";
            }
        }
        f << ++tok << ",1,NIFTY26OCT24000CE,\"NIFTY\",0,2026-10-27,24000,0.05,65,CE,NFO-OPT,NFO\n";
        f << "779521,3045,SBIN,\"STATE BANK OF INDIA\",0,,0,0.1,1,EQ,NSE,NSE\n";
        f << "128028676,500112,SBIN,\"STATE BANK OF INDIA\",0,,0,0.05,1,EQ,BSE,BSE\n";
        f << "519937,2031,M&M,\"MAHINDRA & MAHINDRA\",0,,0,0.1,1,EQ,NSE,NSE\n";
    }
    std::string err;
    const auto rows = read_kite_master(master, err);
    check(err.empty() && rows.size() == 23, "reads the Kite master, quoted names and all");

    LiveUniverseOptions o;
    o.today = parse_day("2026-10-01");
    o.nifty_spot = 23960.0;   // ATM 24000
    o.strikes = 2;
    o.depth_strikes = 1;
    const std::vector<LiveStock> stocks{{"SBIN", "NSE:SBIN-EQ", "banks"}, {"M&M", "NSE:M&M-EQ", "autos"},
                                        {"NOPE", "NSE:NOPE-EQ", "x"}};
    const auto u = build_universe(rows, stocks, o);

    const auto* nifty = find(u, "NIFTY 50");
    check(nifty && nifty->token == kLiveNiftyToken && nifty->fyers == "NSE:NIFTY50-INDEX" && !nifty->depth,
          "NIFTY 50 is streamed as NSE:NIFTY50-INDEX under token 256265, without a book");
    check(find(u, "NIFTY26SEPFUT") == nullptr, "an expired future is not streamed");
    const auto* oct = find(u, "NIFTY26OCTFUT");
    check(oct && oct->fyers == "NSE:NIFTY26OCTFUT" && oct->lot == 65 && oct->depth,
          "the near future maps to NSE:NIFTY26OCTFUT with its lot and a book");
    check(find(u, "NIFTY26NOVFUT") != nullptr && find(u, "NIFTY26DECFUT") == nullptr,
          "exactly the nearest two futures");

    int chain = 0, chain_depth = 0;
    for (const auto& i : u.instruments)
        if (i.group == "NIFTY options") { ++chain; chain_depth += i.depth ? 1 : 0; }
    check(chain == 10, "ATM +/- 2 strikes on the nearest expiry, calls and puts (10 contracts)");
    check(chain_depth == 6, "the book only within ATM +/- 1 (6 contracts)");
    check(find(u, "NIFTY26OCT24000CE") == nullptr, "a later expiry is not mixed into the chain");
    const auto* atm = find(u, "NIFTY26O0624000CE");
    check(atm && atm->fyers == "NSE:NIFTY26O0624000CE" && atm->kind == LiveKind::Call && atm->strike == 24000.0,
          "a weekly option keeps its symbol, prefixed NSE:");
    check(find(u, "NIFTY26O0623700CE") == nullptr && find(u, "NIFTY26O0624300PE") == nullptr,
          "strikes beyond the band are left out");

    const auto* sbin = find(u, "SBIN");
    check(sbin && sbin->token == 779521 && sbin->fyers == "NSE:SBIN-EQ", "SBIN is the NSE row, not the BSE one");
    const auto* mm = find(u, "M&M");
    check(mm && mm->fyers == "NSE:M&M-EQ", "an ampersand symbol survives");
    bool noted_nope = false, noted_bnf = false;
    for (const auto& n : u.notes) {
        noted_nope = noted_nope || n.find("NOPE") != std::string::npos;
        noted_bnf = noted_bnf || n.find("BANKNIFTY") != std::string::npos;
    }
    check(noted_nope && noted_bnf, "what could not be streamed is said, not silently dropped");

    const std::string out = (dir / "universe.csv").string();
    check(write_universe(out, u.instruments), "writes the universe file");
    const auto back = read_universe(out);
    check(back.size() == u.instruments.size(), "reads the same number of instruments back");
    bool same = back.size() == u.instruments.size();
    for (std::size_t i = 0; same && i < back.size(); ++i) {
        same = back[i].token == u.instruments[i].token && back[i].fyers == u.instruments[i].fyers
            && back[i].kind == u.instruments[i].kind && back[i].expiry_day == u.instruments[i].expiry_day
            && back[i].strike == u.instruments[i].strike && back[i].depth == u.instruments[i].depth;
    }
    check(same, "and every field round-trips");
    check(day_text(parse_day("2026-10-06")) == "2026-10-06", "dates round-trip");
    check(ist_today(1790812800) == parse_day("2026-10-01"), "IST today is the IST date, not UTC");

    // The latest close is by date, not by file name: "vendor_pre2015.csv"
    // sorts after "all.csv" and holds the oldest bars.
    const auto part = dir / "bnf1d";
    std::filesystem::create_directories(part);
    {
        std::ofstream a(part / "all.csv");
        a << "time,open,high,low,close,volume\n2026-09-23,1,1,1,56548.9,\n2026-09-24,1,1,1,55438.5,\n";
        std::ofstream v(part / "vendor_pre2015.csv");
        v << "time,open,high,low,close,volume\n2014-12-30,1,1,1,18701.4,\n";
    }
    check(last_close(part.string()) == 55438.5, "the latest close is the latest date, whatever the file is called");

    std::filesystem::remove_all(dir);
    // The market watch's scrip search: master rows into streamable instruments.
    {
        using namespace altair::live;
        LiveKiteRow eq;
        eq.token = 779521; eq.symbol = "SBIN"; eq.name = "STATE BANK OF INDIA"; eq.type = "EQ";
        eq.segment = "NSE"; eq.exchange = "NSE"; eq.tick = 0.05; eq.lot = 1;
        LiveKiteRow opt;
        opt.token = 12345678; opt.symbol = "SBIN26OCT800CE"; opt.name = "SBIN"; opt.type = "CE";
        opt.segment = "NFO-OPT"; opt.exchange = "NFO"; opt.strike = 800; opt.lot = 750; opt.tick = 0.05;
        opt.expiry_day = parse_day("2026-10-27");
        LiveKiteRow mcx;
        mcx.token = 99; mcx.symbol = "GOLD"; mcx.segment = "MCX-FUT"; mcx.exchange = "MCX";
        const auto e = instrument_from_master(eq);
        const auto o = instrument_from_master(opt);
        check(e && e->kind == LiveKind::Equity && e->fyers == "NSE:SBIN-EQ" && e->group == "Watchlist",
              "an NSE equity streams as NSE:<SYM>-EQ in the Watchlist group");
        check(o && o->kind == LiveKind::Call && o->fyers == "NSE:SBIN26OCT800CE" && o->underlying == "SBIN"
                  && o->strike == 800 && o->lot == 750,
              "an NFO option keeps its symbol, underlying, strike and lot");
        check(!instrument_from_master(mcx), "an exchange the feed does not carry is refused, not guessed");

        const auto dir = std::filesystem::temp_directory_path() / "altair_watch_test";
        std::filesystem::create_directories(dir);
        {
            std::ofstream w(dir / "watchlist.csv");
            w << "token,symbol\n779521,SBIN\n12345678,SBIN26OCT800CE\n779521,SBIN\n99,GOLD\n5,NOPE\n";
        }
        const auto toks = read_watchlist((dir / "watchlist.csv").string());
        check(toks.size() == 4 && toks[0] == 779521u, "the watchlist file reads tokens once each, in order");
        std::vector<LiveInstrument> u;
        LiveInstrument have;
        have.token = 779521;
        u.push_back(have);
        std::vector<std::string> notes;
        add_watchlist(u, {eq, opt, mcx}, toks, notes);
        check(u.size() == 2 && u[1].token == 12345678u && notes.size() == 2,
              "added: what is new and streamable; noted: what is not");
        std::filesystem::remove_all(dir);
    }

    std::printf("%s\n", failures == 0 ? "all live universe checks passed" : "live universe checks did not pass");
    return failures == 0 ? 0 : 1;
}
