// Tests for live/instrument_master.hpp: data/instruments.csv built from the
// FYERS public symbol masters, read back by live/universe.hpp.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/instrument_master.hpp>
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

const altair::live::LiveMasterRow* row(const std::vector<altair::live::LiveMasterRow>& rows, const std::string& sym,
                                       const std::string& seg) {
    for (const auto& r : rows)
        if (r.symbol == sym && r.segment == seg) return &r;
    return nullptr;
}

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

}  // namespace

int main() {
    using namespace altair::live;
    namespace fs = std::filesystem;
    std::printf("instrument master from FYERS\n");
    const fs::path dir = fs::temp_directory_path() / "altair_instrument_master_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "fyers_masters");
    const fs::path m = dir / "fyers_masters";

    // Verbatim FYERS rows (instruments/tests/test_fyers_master.cpp, live/tests/test_universe.cpp).
    write(m / "NSE_CM.csv",
          "101000000016921,20 MICRONS LTD,0,1,0.01,INE144J01027,0915-1530|1815-1915:,2026-09-24,,NSE:20MICRONS-EQ,"
          "10,10,16921,20MICRONS,16921,-1.0,XX,101000000016921,None,1,2.0\n"
          "101000000011033,AGASTYA ENE AND INFRA LTD,0,1,0.01,INE753W01010,0915-1530|1815-1915:,2026-10-06,,"
          "NSE:AGASTYAEN-BE,10,10,11033,AGASTYAEN,11033,-1.0,XX,101000000011033,None,0,0.0\r\n"
          "101000000026000,Nifty 50,10,1,0.05,,0915-1530:,2026-10-06,,NSE:NIFTY50-INDEX,10,10,26000,NIFTY50,26000,"
          "-1.0,XX,101000000026000,None,0,0.0\n"
          "101000000026017,India VIX,10,1,0.05,,0915-1530:,2026-10-06,,NSE:INDIAVIX-INDEX,10,10,26017,INDIAVIX,26017,"
          "-1.0,XX,101000000026017,None,0,0.0\n"
          "101000000026099,Nifty Midcap 50,10,1,0.05,,0915-1530:,2026-10-06,,NSE:NIFTYMIDCAP50-INDEX,10,10,26099,"
          "NIFTYMIDCAP50,26099,-1.0,XX,101000000026099,None,0,0.0\n"
          "this,line,is,not,a,master,row\n");
    write(m / "NSE_FO.csv",
          "101126092935000,BANKNIFTY 29 Sep 26 72600 CE,14,30,0.05,,0915-1540|1815-1915:,2026-09-24,1790676600,"
          "NSE:BANKNIFTY26SEP72600CE,10,11,35000,BANKNIFTY,26009,72600.0,CE,101000000026009,None,0,0.0\n"
          "101126092968390,BANKNIFTY 29 Sep 26 FUT,11,30,0.2,,0915-1540|1815-1915:,2026-09-24,1790676600,"
          "NSE:BANKNIFTY26SEPFUT,10,11,68390,BANKNIFTY,26009,-1.0,XX,101000000026009,None,0,0.0\n");
    write(m / "MCX_COM.csv",
          "1120261119573422,CRUDEOIL 26 Nov 19 FUT,11,100,1.0,,0900-2330:,2026-10-20,1795089000,MCX:CRUDEOIL26NOVFUT,"
          "11,20,573422,CRUDEOIL,294,-1.0,XX,1120000000000294,None,0,0.0\n");
    write(m / "NSE_CD.csv",
          "1012260904823299,USDINR 26 Sep 04 96.625 CE,14,1,0.0025,,0900-1700:,2026-10-20,1788525000,"
          "NSE:USDINR26S0496.625CE,10,12,3216,USDINR,1,96.625,CE,101200000000001,None,0,0.0\n");
    write(m / "BSE_CM.csv",
          "1210000000500112,STATE BANK OF INDIA,0,1,0.05,INE062A01020,0915-1530|1815-1915:,2026-10-07,,BSE:SBIN-A,12,10,"
          "500112,SBIN,500112,-1.0,XX,1210000000500112,None,0,0.0\n");
    // BSE_FO.csv deliberately absent.

    const fs::path out = dir / "instruments.csv";
    MasterBuildReport rep;
    std::string err;
    check(build_instrument_master(m, out, rep, err) && err.empty(), "builds data/instruments.csv from the masters");
    check(rep.files == 5 && rep.missing.size() == 1 && rep.missing[0] == "BSE_FO", "a missing master is reported");
    check(rep.rows == 10 && rep.skipped == 1, "every understood row written, the junk line skipped");
    check(!fs::exists(dir / "instruments.csv.building"), "no temporary file left behind");

    const auto rows = read_instrument_master(out.string(), err);
    check(err.empty() && rows.size() == 10, "the universe reader reads it back");

    const auto* eq = row(rows, "20MICRONS", "NSE");
    check(eq && eq->token == 16921u * 256 + 1 && eq->exchange_token == 16921 && eq->name == "20 MICRONS LTD"
              && eq->type == "EQ" && eq->exchange == "NSE" && eq->lot == 1,
          "NSE cash: -EQ dropped, token = exchange token x 256 + 1, company name");
    check(row(rows, "AGASTYAEN-BE", "NSE") != nullptr, "an NSE scrip outside the EQ series keeps its series");
    const auto* bse = row(rows, "SBIN", "BSE");
    check(bse && bse->token == 128028676u && bse->exchange == "BSE", "BSE cash: group dropped, token x 256 + 4");

    const auto* nifty = row(rows, "NIFTY 50", "INDICES");
    check(nifty && nifty->token == kLiveNiftyToken, "NIFTY 50 keeps the fixed token 256265");
    const auto* vix = row(rows, "INDIA VIX", "INDICES");
    check(vix && vix->token == kLiveVixToken, "INDIA VIX keeps the fixed token 264969");
    const auto* mid = row(rows, "NIFTYMIDCAP50", "INDICES");
    check(mid && mid->token == 26099u * 256 + 9, "any other index: FYERS exchange token x 256 + 9");

    const auto* ce = row(rows, "BANKNIFTY26SEP72600CE", "NFO-OPT");
    check(ce && ce->token == 35000u * 256 + 2 && ce->name == "BANKNIFTY" && ce->type == "CE" && ce->strike == 72600.0
              && ce->lot == 30 && ce->tick == 0.05 && ce->expiry_day == parse_day("2026-09-29"),
          "NFO option: underlying, strike, lot, tick and the IST expiry date");
    const auto* fut = row(rows, "BANKNIFTY26SEPFUT", "NFO-FUT");
    check(fut && fut->type == "FUT" && fut->strike == 0.0 && fut->tick == 0.2, "NFO future");

    const auto* crude = row(rows, "CRUDEOIL26NOVFUT", "MCX-FUT");
    check(crude && crude->token == 573422u * 256 + 7 && crude->lot == 1 && crude->exchange == "MCX",
          "MCX: lot 1 (an order is in lots; live/contract_units.hpp knows the units)");
    const auto* usd = row(rows, "USDINR26S0496.625CE", "CDS-OPT");
    check(usd && usd->token == 823299u && usd->strike == 96.625 && usd->lot == 1, "currency option, token x 256 + 3");

    check(!instrument_master_stale(m, out), "fresh after a build");
    fs::last_write_time(m / "NSE_FO.csv", fs::last_write_time(out) + std::chrono::hours(1));
    check(instrument_master_stale(m, out), "stale once a master is newer");

    MasterBuildReport none;
    check(!build_instrument_master(dir / "nowhere", dir / "other.csv", none, err) && !fs::exists(dir / "other.csv"),
          "no masters: refused, nothing written");

    fs::remove_all(dir);
    std::printf("%s\n", failures == 0 ? "all instrument master checks passed" : "instrument master checks did not pass");
    return failures == 0 ? 0 : 1;
}
