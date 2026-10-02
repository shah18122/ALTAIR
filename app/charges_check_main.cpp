// altair_charges_check -- does config/charges.toml price a real contract note
// to the paisa, head by head? (app/charges_check.hpp says how.)
//
//   altair_charges_check --note NOTE.csv [--charges FILE] [--tolerance-paise N] [--out FILE]
//
// Writes the finding to data/verified/charges_check.json (kept out of git),
// which altair_readiness reads. Exit 0: every order agreed; 1: something
// differed or went unmatched; 2: the note or the schedule could not be read.

#include <app/charges_check.hpp>
#include <risk/charges_toml.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace cc = altair::charges_check;

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

void usage(const char* exe) {
    std::printf(
        "  The paper engine's expenses against a contract note, head by head.\n\n"
        "    %s --note NOTE.csv [--charges FILE] [--tolerance-paise N] [--out FILE] [--root DIR]\n\n"
        "    --note      the note, normalised: order_id,date,symbol,segment,side,qty,price,brokerage,stt,\n"
        "                exchange_txn,sebi,stamp,ipft,gst[,time] (rupees; segment FUT/OPT/CASH; side BUY/SELL)\n"
        "    --charges   the schedule to test (default <root>/config/charges.toml)\n"
        "    --tolerance-paise N  a head within N paise of the note's agrees (default 0)\n"
        "    --out       the finding (default <root>/data/verified/charges_check.json)\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = ALTAIR_SOURCE_DIR, note, charges, out;
    long long tol = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--note" && has) { note = argv[++i]; continue; }
        if (a == "--charges" && has) { charges = argv[++i]; continue; }
        if (a == "--tolerance-paise" && has) { tol = std::atoll(argv[++i]); continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    if (note.empty() || tol < 0) { usage(argv[0]); return 2; }
    if (charges.empty()) charges = root / "config/charges.toml";
    if (out.empty()) out = root / "data/verified/charges_check.json";

    std::string text;
    {
        std::ifstream f(note, std::ios::binary);
        if (!f) { std::printf("cannot read %s\n", note.string().c_str()); return 2; }
        std::ostringstream o;
        o << f.rdbuf();
        text = o.str();
    }
    std::vector<std::string> errors;
    const auto orders = cc::parse_note(text, errors);
    for (const auto& e : errors) std::printf("note: %s\n", e.c_str());
    if (!errors.empty()) { std::printf("the note has rows that could not be read: fix them first, nothing was checked\n"); return 2; }
    if (orders.empty()) { std::printf("the note has no trades: an empty note checks nothing\n"); return 2; }

    std::vector<altair::ChargeSchedule> schedules;
    const auto rep = altair::load_charges_file(charges.string().c_str(), schedules);
    if (!rep) { std::printf("%s did not load: %s\n", charges.string().c_str(), altair::charges_error_text(rep.error())); return 2; }
    const bool flagged_verified = rep->verified;
    for (auto& s : schedules) s.verified = true;   // testing the schedule is the point

    const auto r = cc::check(orders, schedules, tol);
    std::int64_t first = 0, last = 0;
    std::size_t rows = 0;
    for (const auto& o : orders) {
        first = first == 0 ? o.day : std::min(first, o.day);
        last = std::max(last, o.day);
        rows += o.rows;
    }
    std::printf("%zu order(s) (%zu note rows), %s to %s; tolerance %lld paise per head\n", orders.size(), rows,
                altair::live::day_text(first).c_str(), altair::live::day_text(last).c_str(), tol);
    std::printf("agreed %zu of %zu; %zu discrepancy(ies); net charge delta %+.2f rupees (engine minus note; display only)%s\n",
                r.report.agreed, orders.size(), r.report.discrepancies.size(), static_cast<double>(r.report.net_charge_delta_paise) / 100.0,
                r.unpriced ? (" -- " + std::to_string(r.unpriced) + " order(s) the schedule could not price").c_str() : "");
    for (const auto& d : r.report.discrepancies)
        std::printf("  %-14s %-12s %-22s %s %+.2f\n", altair::finding_text(d.finding), d.order_id.c_str(), d.symbol.c_str(),
                    d.head == altair::Head::None ? "" : altair::head_text(d.head), static_cast<double>(d.delta_paise) / 100.0);

    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f << "{\"ok\": " << (r.report.ok() && r.unpriced == 0 ? "true" : "false") << ", \"note\": " << jstr(note.string())
      << ", \"charges\": " << jstr(charges.string()) << ", \"schedule_flagged_verified\": " << (flagged_verified ? "true" : "false")
      << ", \"first_day\": " << jstr(altair::live::day_text(first)) << ", \"last_day\": " << jstr(altair::live::day_text(last))
      << ", \"orders\": " << orders.size() << ", \"note_rows\": " << rows << ", \"agreed\": " << r.report.agreed
      << ", \"unpriced\": " << r.unpriced << ", \"tolerance_paise\": " << tol << ", \"net_charge_delta_paise\": "
      << r.report.net_charge_delta_paise << ", \"checked_unix\": "
      << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
      << ", \"discrepancies\": [";
    for (std::size_t i = 0; i < r.report.discrepancies.size(); ++i) {
        const auto& d = r.report.discrepancies[i];
        f << (i ? ", " : "") << "{\"finding\": " << jstr(altair::finding_text(d.finding)) << ", \"order_id\": " << jstr(d.order_id)
          << ", \"symbol\": " << jstr(d.symbol) << ", \"head\": " << jstr(d.head == altair::Head::None ? "" : altair::head_text(d.head))
          << ", \"delta_paise\": " << d.delta_paise << "}";
    }
    f << "]}\n";
    std::printf("-> %s\n", out.string().c_str());
    return r.report.ok() && r.unpriced == 0 ? 0 : 1;
}
