// Tests for app/fyers_chain.hpp -- the FYERS chain record: contracts, closes, reload.

#include <app/fyers_chain.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fc = altair::fyers_chain;
namespace da = altair::data_audit;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << text;
}

// Real rows of FYERS's NSE_FO master (instruments/tests/test_fyers_master.cpp),
// the BANKNIFTY 29-Sep-2026 expiry, plus a cash row that must be ignored.
std::string master() {
    std::string s;
    s += "101000000016921,20 MICRONS LTD,0,1,0.01,INE144J01027,0915-1530|1815-1915:,"
         "2026-09-24,,NSE:20MICRONS-EQ,10,10,16921,20MICRONS,16921,-1.0,XX,"
         "101000000016921,None,1,2.0\n";
    s += "101126092935000,BANKNIFTY 29 Sep 26 72600 CE,14,30,0.05,,0915-1540|1815-1915:,"
         "2026-09-24,1790676600,NSE:BANKNIFTY26SEP72600CE,10,11,35000,BANKNIFTY,26009,"
         "72600.0,CE,101000000026009,None,0,0.0\n";
    s += "101126092968390,BANKNIFTY 29 Sep 26 FUT,11,30,0.2,,0915-1540|1815-1915:,"
         "2026-09-24,1790676600,NSE:BANKNIFTY26SEPFUT,10,11,68390,BANKNIFTY,26009,"
         "-1.0,XX,101000000026009,None,0,0.0\n";
    s += "not,a,master,row\n";
    return s;
}

std::int64_t day(int y, unsigned m, unsigned d) { return da::audit_days_from_civil(y, m, d); }

/// UTC ns of an IST wall-clock minute.
std::int64_t ist(std::int64_t d, int hh, int mm) {
    return ((d * 86'400) + hh * 3600 + mm * 60 - fc::kIstOffsetS) * 1'000'000'000LL;
}

void test_master() {
    std::istringstream in(master());
    std::size_t refused = 0;
    const auto rows = fc::read_fo_master(in, {"BANKNIFTY"}, &refused);
    check(rows.size() == 2 && refused == 1, "two BANKNIFTY contracts read, the cash row skipped, the junk row refused");
    if (rows.size() != 2) { return; }
    check(rows[0].kind == "CE" && rows[0].strike == 72600.0 && rows[0].lot == 30
              && rows[0].ticker == "NSE:BANKNIFTY26SEP72600CE",
          "the option: CE, strike 72600, lot 30, the FYERS ticker");
    check(rows[1].kind == "FUT" && rows[1].strike == 0.0, "the future: FUT, no strike");
    check(rows[0].expiry == day(2026, 9, 29), "expiry 1790676600 s is the IST day 2026-09-29");
}

void test_pick() {
    // Weeklies on 6, 13, 20 Oct and monthlies 27 Oct and 24 Nov; strikes every 50.
    std::vector<fc::Contract> listed;
    const std::int64_t exps[] = {day(2026, 10, 6), day(2026, 10, 13), day(2026, 10, 20), day(2026, 10, 27), day(2026, 11, 24)};
    for (const auto e : exps) {
        for (int k = 24'000; k <= 26'000; k += 50) {
            for (const char* t : {"CE", "PE"}) {
                listed.push_back({"NSE:X" + std::to_string(e) + t + std::to_string(k), "NIFTY", t, e, static_cast<double>(k), 65});
            }
        }
    }
    listed.push_back({"NSE:NIFTYOCTFUT", "NIFTY", "FUT", day(2026, 10, 27), 0.0, 65});
    listed.push_back({"NSE:OLDFUT", "NIFTY", "FUT", day(2026, 9, 29), 0.0, 65});
    fc::Pick p;
    p.nearest = 3;
    p.monthlies = 1;
    p.strikes = 2;
    const auto got = fc::pick(listed, "NIFTY", 25'010.0, day(2026, 10, 1), p);
    std::set<std::int64_t> exp;
    std::set<double> ks;
    std::size_t futs = 0;
    for (const auto& c : got) {
        if (c.kind == "FUT") { ++futs; continue; }
        exp.insert(c.expiry);
        ks.insert(c.strike);
    }
    check(exp.size() == 4 && !exp.contains(day(2026, 11, 24)), "the three nearest expiries plus the October monthly, not November");
    check(ks == std::set<double>{24'900, 24'950, 25'000, 25'050, 25'100}, "the five strikes nearest a 25,010 spot");
    check(futs == 1, "the live future kept, the expired one dropped");
    check(got.size() == 1 + 4 * 5 * 2, "calls and puts of each strike of each expiry");
}

void test_day_close() {
    const auto d = day(2026, 10, 27);
    std::vector<fc::Bar> bars{{ist(d, 9, 15), 0, 0, 0, 100.0, 1}, {ist(d, 15, 28), 0, 0, 0, 120.0, 1},
                              {ist(d, 15, 29), 0, 0, 0, 121.5, 1}, {ist(d, 15, 30), 0, 0, 0, 999.0, 1}};
    const auto c = fc::day_close(bars);
    check(c && c->close == 121.5 && c->last_minute == 15 * 60 + 29 && c->bars == 3,
          "the close is the 15:29 bar; a 15:30 print is not a session bar");
    const std::vector<fc::Bar> early{{ist(d, 9, 15), 0, 0, 0, 100.0, 1}, {ist(d, 13, 0), 0, 0, 0, 90.0, 1}};
    check(!fc::day_close(early), "a contract last traded at 13:00 has no close");
    check(fc::ist_day(ist(d, 0, 10)) == d && fc::ist_day(ist(d, 23, 50)) == d, "IST day boundaries, not UTC");
}

void test_load() {
    const fs::path root = fs::temp_directory_path() / "altair_fyers_chain_test";
    fs::remove_all(root);
    const fs::path chain = root / "fyers_chain", snap = root / "chain_snap";
    // A saved day: monthly 27 Oct future + options, and a weekly 20 Oct option.
    write(chain / "2026-10-16" / "NIFTY.csv",
          fc::closes_header()
              + "NSE:NIFTY26OCTFUT,FUT,2026-10-27,0.00,25100.00,15:29,375\n"
                "NSE:NIFTY26OCT25100CE,CE,2026-10-27,25100.00,210.50,15:29,370\n"
                "NSE:NIFTY26OCT25100PE,PE,2026-10-27,25100.00,190.25,15:29,368\n"
                "NSE:NIFTY26OCT32000CE,CE,2026-10-27,32000.00,0.05,15:25,4\n"
                "NSE:NIFTY26O2025100CE,CE,2026-10-20,25100.00,120.00,15:29,375\n"
                "NSE:NIFTY50-INDEX,SPOT,,0.00,25080.00,15:29,375\n");
    // A day only the 15:27 REST snapshot has.
    write(snap / "2026-10-19" / "1527.csv",
          "snap_time,fyers,underlying,kind,expiry,strike,ltp,bid,ask,volume,open,high,low,prev_close,exch_time\n"
          "15:27,NSE:NIFTY26OCTFUT,NIFTY,future,2026-10-27,,25200,0,0,0,0,0,0,0,0\n"
          "15:27,NSE:NIFTY26OCT25200PE,NIFTY,put,2026-10-27,25200,150.5,0,0,0,0,0,0,0,0\n"
          "15:27,NSE:BANKNIFTY26OCTFUT,BANKNIFTY,future,2026-10-27,,56000,0,0,0,0,0,0,0,0\n");
    // A snapshot for the saved day is ignored: the bars win.
    write(snap / "2026-10-16" / "1527.csv",
          "snap_time,fyers,underlying,kind,expiry,strike,ltp\n15:27,NSE:NIFTY26OCTFUT,NIFTY,future,2026-10-27,,1\n");

    fc::LoadReport rep;
    const auto days = fc::load_dir(chain, "NIFTY", rep, true, snap);
    check(days.size() == 2 && rep.days == 1 && rep.snapshot_days == 1, "one saved day and one snapshot-only day");
    const auto e = day(2026, 10, 27);
    const auto d1 = days.find(day(2026, 10, 16));
    check(d1 != days.end() && d1->second.fut.at(e) == 25100.0, "the future's close from the saved bars, not the snapshot");
    if (d1 != days.end()) {
        const auto& legs = d1->second.opt.at(e);
        check(legs.size() == 1 && legs.at(25100.0).call == 210.5 && legs.at(25100.0).put == 190.25,
              "the ATM straddle; a strike 27 % away is outside the bhavcopy's +-20 %");
        check(!d1->second.opt.contains(day(2026, 10, 20)), "monthly only: the weekly is left out");
    }
    const auto d2 = days.find(day(2026, 10, 19));
    check(d2 != days.end() && d2->second.fut.at(e) == 25200.0 && std::isnan(d2->second.opt.at(e).at(25200.0).call)
              && d2->second.opt.at(e).at(25200.0).put == 150.5,
          "the snapshot day: only NIFTY's rows, put without a call");

    fc::LoadReport all;
    const auto weekly = fc::load_dir(chain, "NIFTY", all, false);
    check(weekly.size() == 1 && weekly.begin()->second.opt.contains(day(2026, 10, 20)), "every expiry when asked: the weekly is there");
    fs::remove_all(root);
}

} // namespace

int main() {
    std::printf("FYERS chain\n");
    test_master();
    test_pick();
    test_day_close();
    test_load();
    std::printf("FYERS chain: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
