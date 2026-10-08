// The charges check: a note priced by the schedule itself agrees to the
// paisa; one head moved by a rupee is caught on that head, signed; a
// tolerance absorbs rounding but not a wrong rate; a date no schedule covers
// is reported, not priced at zero; an order split over fills is one order;
// and an unreadable note checks nothing.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/charges_check.hpp>
#include <risk/charges_toml.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair;
namespace cc = altair::charges_check;

std::string rupees(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

/// Note rows for one order priced by the schedule: the charges on the first
/// row, the remaining fills (if split) with none -- brokerage is per order.
std::string order_rows(const std::vector<ChargeSchedule>& sch, const char* id, const char* date, const char* sym, const char* seg,
                       const char* side, std::int64_t qty, double px, int split = 1, double stt_bump = 0.0) {
    const std::int64_t day = live::parse_day(date);
    const Segment s = std::string(seg) == "FUT" ? Segment::Fut : std::string(seg) == "OPT" ? Segment::Opt : Segment::Cash;
    const auto c = demo_costs::fill(s, std::string(side) == "SELL" ? Side::Sell : Side::Buy, static_cast<double>(qty), px,
                                    day * 86400 + 11 * 3600, sch);
    std::string out;
    for (int k = 0; k < split; ++k) {
        const std::int64_t q = qty / split + (k == 0 ? qty % split : 0);
        out += std::string(id) + "," + date + "," + sym + "," + seg + "," + side + "," + std::to_string(q) + "," + rupees(px) + ",";
        if (k == 0)
            out += rupees(c.brokerage) + "," + rupees(c.stt + stt_bump) + "," + rupees(c.exchange) + "," + rupees(c.sebi) + ","
                 + rupees(c.stamp) + "," + rupees(c.ipft) + "," + rupees(c.gst);
        else out += "0,0,0,0,0,0,0";
        out += ",11:00:00\n";
    }
    return out;
}

} // namespace

int main() {
    std::printf("charges check\n");
    std::vector<ChargeSchedule> sch;
    const auto rep = load_charges_file(ALTAIR_SOURCE_DIR "/config/charges.toml", sch);
    check(rep.has_value() && !sch.empty(), "config/charges.toml loads");
    if (!rep || sch.empty()) { std::printf("cannot go on without a schedule\n"); return 1; }
    for (auto& s : sch) s.verified = true;
    const std::string header = "order_id,date,symbol,segment,side,qty,price,brokerage,stt,exchange_txn,sebi,stamp,ipft,gst,time\n";

    const std::string good = header + order_rows(sch, "A1", "2026-09-24", "NIFTY26SEPFUT", "FUT", "BUY", 75, 23250.0)
                           + order_rows(sch, "A2", "2026-09-24", "NIFTY26SEP23350CE", "OPT", "SELL", 130, 81.10, 2)
                           + order_rows(sch, "A3", "2026-09-24", "SBIN", "CASH", "BUY", 10, 812.35);
    std::vector<std::string> errors;
    const auto orders = cc::parse_note(good, errors);
    check(errors.empty() && orders.size() == 3, "a clean note: three orders");
    check(orders.size() == 3 && orders[1].rows == 2 && orders[1].qty == 130, "an order split over two fills is summed into one");
    const auto r = cc::check(orders, sch);
    check(r.report.ok() && r.report.agreed == 3 && r.unpriced == 0, "priced by the schedule itself, every head agrees to the paisa");

    const std::string off = header + order_rows(sch, "B1", "2026-09-24", "NIFTY26SEP23350CE", "OPT", "SELL", 65, 81.10, 1, 1.00);
    errors.clear();
    const auto r2 = cc::check(cc::parse_note(off, errors), sch);
    check(!r2.report.ok() && r2.report.discrepancies.size() == 1 && r2.report.discrepancies[0].finding == Finding::ChargeMismatch
              && r2.report.discrepancies[0].head == Head::Stt && r2.report.discrepancies[0].delta_paise == -100,
          "STT a rupee higher on the note: caught on STT, signed engine minus note (-100 paise)");
    check(cc::check(cc::parse_note(off, errors), sch, 150).report.ok(), "a 150-paise tolerance absorbs it");
    check(!cc::check(cc::parse_note(off, errors), sch, 50).report.ok(), "a 50-paise one does not");

    const std::string old = header + order_rows(sch, "C1", "2026-09-24", "NIFTY26SEPFUT", "FUT", "BUY", 75, 23250.0)
                          + "C2,1990-01-02,NIFTY90JANFUT,FUT,BUY,75,1000.00,20,1,1,1,1,1,1,11:00:00\n";
    errors.clear();
    const auto r3 = cc::check(cc::parse_note(old, errors), sch);
    check(r3.unpriced == 1 && !r3.report.ok(), "a date no schedule covers: reported, not priced at zero");

    errors.clear();
    (void)cc::parse_note(header + "D1,2026-09-24,X,FUT,HOLD,75,100,0,0,0,0,0,0,0,11:00:00\n", errors);
    check(errors.size() == 1 && errors[0].find("line 2") != std::string::npos, "an unreadable row is named by its line");
    errors.clear();
    (void)cc::parse_note("order_id,date,symbol\n", errors);
    check(!errors.empty(), "a note without the charge columns is refused whole");

    // The venue: a BSE fill pays BSE's transaction charge; intraday equity
    // brokerage is Rs 20 or 0.03 %, whichever is lower.
    {
        const std::int64_t at = live::parse_day("2026-09-24") * 86400 + 11 * 3600;
        const auto n = demo_costs::fill(Segment::Cash, Side::Buy, 100, 1000.0, at, sch);
        const auto b = demo_costs::fill(Segment::Cash, Side::Buy, 100, 1000.0, at, sch, Exchange::BSE);
        check(n.priced && b.priced && std::fabs(n.exchange - 100000.0 * 0.0000297) < 0.01
                  && std::fabs(b.exchange - 100000.0 * 0.0000375) < 0.01 && b.total > n.total,
              "a BSE equity fill pays BSE's 0.00375 %, an NSE one NSE's 0.00297 %");
        const auto small = demo_costs::fill(Segment::Cash, Side::Buy, 10, 812.35, at, sch);
        check(std::fabs(n.brokerage - 20.0) < 1e-9 && std::fabs(small.brokerage - 8123.5 * 0.0003) < 0.01,
              "intraday equity brokerage: Rs 20 on Rs 1 lakh, 0.03 % on Rs 8,123.50");
    }
    std::printf("%s\n", failures == 0 ? "all charges check checks passed" : "charges check checks did not pass");
    return failures == 0 ? 0 : 1;
}
