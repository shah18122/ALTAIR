// Tests for app/bhavcopy.hpp -- both NSE F&O bhavcopy formats, from fixtures.

#include <app/bhavcopy.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bc = altair::bhavcopy;
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

std::string legacy() {
    std::string s = "INSTRUMENT,SYMBOL,EXPIRY_DT,STRIKE_PR,OPTION_TYP,OPEN,HIGH,LOW,CLOSE,SETTLE_PR,CONTRACTS,VAL_INLAKH,"
                    "OPEN_INT,CHG_IN_OI,TIMESTAMP,\n";
    const auto row = [&](const char* ins, const char* sym, const char* exp, const char* k, const char* t, const char* close,
                         const char* settle, const char* contracts) {
        s += std::string{ins} + ',' + sym + ',' + exp + ',' + k + ',' + t + ",0,0,0," + close + ',' + settle + ',' + contracts
           + ",0,0,0,03-FEB-2020,\n";
    };
    row("FUTIDX", "NIFTY", "27-Feb-2020", "0", "XX", "12120.50", "12118.00", "100000");
    row("FUTIDX", "NIFTY", "26-Mar-2020", "0", "XX", "12160.00", "12158.00", "20000");
    row("OPTIDX", "NIFTY", "06-Feb-2020", "12100", "CE", "90.00", "90.00", "90000");    // weekly
    row("OPTIDX", "NIFTY", "27-Feb-2020", "12100", "CE", "180.50", "181.00", "5000");
    row("OPTIDX", "NIFTY", "27-Feb-2020", "12100", "PE", "160.25", "160.00", "4000");
    row("OPTIDX", "NIFTY", "27-Feb-2020", "12200", "CE", "0.00", "130.00", "0");         // untraded
    row("OPTIDX", "NIFTY", "27-Feb-2020", "20000", "CE", "0.05", "0.05", "10");          // 65 % away
    row("OPTIDX", "BANKNIFTY", "27-Feb-2020", "31000", "CE", "400.00", "400.00", "900"); // another symbol
    row("OPTSTK", "RELIANCE", "27-Feb-2020", "1500", "CE", "20.00", "20.00", "100");
    return s;
}

std::string udiff() {
    const std::vector<std::string> head{"TradDt", "BizDt", "Sgmt", "Src", "FinInstrmTp", "FinInstrmId", "ISIN", "TckrSymb",
                                        "SctySrs", "XpryDt", "FininstrmActlXpryDt", "StrkPric", "OptnTp", "FinInstrmNm",
                                        "OpnPric", "HghPric", "LwPric", "ClsPric", "LastPric", "PrvsClsgPric", "UndrlygPric",
                                        "SttlmPric", "OpnIntrst", "ChngInOpnIntrst", "TtlTradgVol", "TtlTrfVal",
                                        "TtlNbOfTxsExctd", "SsnId", "NewBrdLotQty", "Rmks", "Rsvd1", "Rsvd2", "Rsvd3", "Rsvd4"};
    std::string s;
    for (std::size_t k = 0; k < head.size(); ++k) { s += (k ? "," : "") + head[k]; }
    s += '\n';
    const auto row = [&](std::map<std::string, std::string> v) {
        v["TradDt"] = "2024-07-08";
        v["TckrSymb"] = v.count("TckrSymb") ? v["TckrSymb"] : "NIFTY";
        for (std::size_t k = 0; k < head.size(); ++k) { s += (k ? "," : "") + (v.count(head[k]) ? v[head[k]] : std::string{}); }
        s += '\n';
    };
    row({{"FinInstrmTp", "IDF"}, {"XpryDt", "2024-07-25"}, {"ClsPric", "24400.50"}, {"SttlmPric", "24398.00"}, {"TtlTradgVol", "100000"}});
    row({{"FinInstrmTp", "IDO"}, {"XpryDt", "2024-07-25"}, {"StrkPric", "24400"}, {"OptnTp", "CE"}, {"ClsPric", "250.00"},
         {"SttlmPric", "250.00"}, {"TtlTradgVol", "50000"}});
    row({{"FinInstrmTp", "IDO"}, {"XpryDt", "2024-07-25"}, {"StrkPric", "24400"}, {"OptnTp", "PE"}, {"ClsPric", "230.00"},
         {"SttlmPric", "230.00"}, {"TtlTradgVol", "45000"}});
    row({{"FinInstrmTp", "IDO"}, {"XpryDt", "2024-07-11"}, {"StrkPric", "24400"}, {"OptnTp", "CE"}, {"ClsPric", "120.00"},
         {"SttlmPric", "120.00"}, {"TtlTradgVol", "900000"}});   // weekly
    return s;
}

} // namespace

int main() {
    std::printf("NSE bhavcopy\n");
    const fs::path dir = fs::temp_directory_path() / "altair_bhavcopy_test";
    fs::remove_all(dir);
    write(dir / "2020" / "fo03FEB2020bhav.csv", legacy());
    write(dir / "2024" / "BhavCopy_NSE_FO_0_0_0_20240708_F_0000.csv", udiff());
    write(dir / "notes.csv", "a,b,c\n1,2,3\n");
    bc::LoadReport rep;
    const auto days = bc::load_dir(dir, "NIFTY", rep);
    check(rep.files == 2 && rep.legacy == 1 && rep.udiff == 1 && rep.unknown == 1,
          "both formats are recognised by their headers; a file with neither is counted and skipped");
    const std::int64_t d20 = da::audit_days_from_civil(2020, 2, 3), feb = da::audit_days_from_civil(2020, 2, 27);
    const std::int64_t d24 = da::audit_days_from_civil(2024, 7, 8), jul = da::audit_days_from_civil(2024, 7, 25);
    check(days.size() == 2 && days.contains(d20) && days.contains(d24), "one day from each file");
    if (days.size() != 2 || !days.contains(d20) || !days.contains(d24)) { return 1; }
    const auto& a = days.at(d20);
    check(a.fut.size() == 2 && a.fut.at(feb) == 12120.50, "legacy futures: monthly contracts at their traded close");
    check(a.opt.size() == 1 && a.opt.contains(feb), "the weekly expiry is dropped: monthly contracts only");
    const auto& strikes = a.opt.at(feb);
    check(strikes.size() == 1 && strikes.at(12100.0).call == 180.50 && strikes.at(12100.0).put == 160.25,
          "legacy options: both legs at their traded closes");
    check(rep.untraded == 1 && !strikes.contains(12200.0),
          "an untraded strike has no price -- its settlement is NSE's model, not a market");
    check(!strikes.contains(20000.0), "a strike 65 % from the future is not kept");
    const auto& b = days.at(d24);
    check(b.fut.at(jul) == 24400.50 && b.opt.at(jul).at(24400.0).call == 250.0 && b.opt.at(jul).at(24400.0).put == 230.0
              && b.opt.size() == 1,
          "UDiFF: the same fields from the ISO-tag columns, weekly dropped");
    check(rep.option_rows == 4 && rep.future_rows == 3, "only NIFTY index futures and options are read");
    fs::remove_all(dir);
    std::printf("NSE bhavcopy: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
