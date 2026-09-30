// app/data_audit_main.cpp -- altair_data_audit: verify dataset/ across
// timeframes, against its other sources, and against broker candles.
//
//   altair_data_audit [--dataset DIR] [--out DIR] [--broker NAME=DIR]...
//                     [--max-issues N] [--no-merged]
//
// Reads DIR/<segment>/<instrument>/<tf>/*.csv (tf = 1m 5m 15m 60m 1d) and any
// DIR/<segment>/<instrument>/_superseded_*/ folders (other sources). A broker
// directory has the same layout (ops/broker_audit.ps1 fills
// data/broker_audit/fyers and data/broker_audit/kite).
//
// Writes, under --out (default data/verified):
//   data_audit.xlsx          Summary sheet + one sheet per instrument
//   data_audit.txt           the same summary as text
//   merged/<seg>_<inst>_<tf>.csv   one sorted, de-duplicated file per series
//
// Read-only against dataset/ and the broker folders. No network.
// See app/data_audit.hpp for what each check means.

#include <app/data_audit.hpp>
#include <app/xlsx_writer.hpp>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace da = altair::data_audit;
namespace fs = std::filesystem;
using altair::xlsx::XlsxCell;
using altair::xlsx::XlsxRow;
using altair::xlsx::XlsxSheet;

constexpr std::array<int, 5> kTimeframes{1, 5, 15, 60, da::kDailyTf};

struct SeriesResult {
    int tf{};
    da::AuditSeries series;
    da::AuditIntegrity integrity;
};

struct CompareResult {
    std::string kind;          ///< "cross-timeframe", "other source", "broker"
    std::string reference;     ///< stored series, e.g. "60m"
    std::string other;         ///< e.g. "rebuilt from 1m", "FYERS 60m"
    da::AuditCompare cmp;
};

struct Instrument {
    std::string segment;
    std::string name;
    std::vector<std::pair<std::string, std::string>> provenance;   ///< from _manifest.json
    std::vector<SeriesResult> series;
    std::vector<CompareResult> compares;
    [[nodiscard]] std::string id() const { return segment + "_" + name; }
    [[nodiscard]] const SeriesResult* tf(int t) const {
        for (const auto& s : series) if (s.tf == t) return &s;
        return nullptr;
    }
};

[[nodiscard]] std::string price_text(double v) {
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return r.ec == std::errc{} ? std::string{buf, r.ptr} : std::string{};
}

[[nodiscard]] std::string integrity_status(const SeriesResult& s) {
    const auto& a = s.series;
    const auto& i = s.integrity;
    if (a.parse_errors > 0 || a.conflicts > 0 || i.bad_ohlc > 0) return "FAIL";
    if (a.duplicates > 0 || a.unsorted > 0 || i.outside_session > 0 || i.off_grid > 0 || i.short_days > 0
        || i.weekend_days > 0 || a.seconds_floored > 0)
        return "WARN";
    return "PASS";
}

[[nodiscard]] std::string compare_status(const CompareResult& r) {
    const auto& c = r.cmp;
    if (c.compared == 0) return "NO OVERLAP";
    if (c.mismatch_bars > 0) {
        const bool close_only = c.field_mismatch[0] == 0 && c.field_mismatch[1] == 0 && c.field_mismatch[2] == 0;
        if (close_only && r.kind == "cross-timeframe" && r.reference == "1d")
            return "CLOSE ONLY (NSE official close)";
        return "FAIL";
    }
    if (c.rounding_bars > 0 || c.only_other > 0 || c.only_reference > 0) return "WARN";
    return "PASS";
}

void write_merged(const fs::path& dir, const Instrument& ins, const SeriesResult& s) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (ins.id() + "_" + da::audit_tf_name(s.tf) + ".csv");
    std::ofstream out(file.string() + ".tmp", std::ios::binary | std::ios::trunc);
    const bool daily = s.tf == da::kDailyTf;
    out << "time,open,high,low,close,volume" << (s.series.has_oi ? ",oi" : "") << "\n";
    for (const auto& b : s.series.bars) {
        out << da::format_audit_time(b.t, daily) << ',' << price_text(b.o) << ',' << price_text(b.h) << ','
            << price_text(b.l) << ',' << price_text(b.c) << ',';
        if (!std::isnan(b.v)) out << price_text(b.v);
        if (s.series.has_oi) {
            out << ',';
            if (!std::isnan(b.oi)) out << price_text(b.oi);
        }
        out << '\n';
    }
    out.close();
    if (!out) {
        std::printf("  could not write %s\n", file.string().c_str());
        fs::remove(file.string() + ".tmp", ec);
        return;
    }
    fs::rename(file.string() + ".tmp", file, ec);
}

/// The top-level string fields of a dataset manifest that say where the data
/// came from (source, trust, derivation, volume, dedupe). A minimal reader:
/// "key": "value" pairs at any depth are taken for those keys only.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> read_provenance(const fs::path& file) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (const char* key : {"source", "trust", "derivation", "volume", "dedupe"}) {
        const std::string needle = std::string{"\""} + key + "\"";
        const std::size_t at = text.find(needle);
        if (at == std::string::npos) continue;
        std::size_t p = text.find(':', at + needle.size());
        if (p == std::string::npos) continue;
        ++p;
        while (p < text.size() && (text[p] == ' ' || text[p] == '\n' || text[p] == '\r' || text[p] == '\t')) ++p;
        if (p >= text.size() || text[p] != '"') continue;
        std::string value;
        for (++p; p < text.size() && text[p] != '"'; ++p) {
            if (text[p] == '\\' && p + 1 < text.size()) ++p;
            value.push_back(text[p]);
        }
        out.emplace_back(key, value);
    }
    return out;
}

/// Load an other-source folder: each file is read alone, its timeframe
/// inferred, and files of one timeframe merged.
[[nodiscard]] std::map<int, da::AuditSeries> load_other_source(const fs::path& dir, std::int64_t close) {
    std::map<int, da::AuditSeries> by_tf;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".csv" && e.path().filename().string()[0] != '.')
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        da::AuditSeries one;
        one.session_close = close;
        std::ifstream in(p, std::ios::binary);
        da::load_audit_csv(one, in, p.filename().string());
        da::finish_audit_series(one);
        const auto tf = da::infer_audit_tf(one);
        if (!tf) continue;
        da::AuditSeries& into = by_tf[*tf];
        into.tf = *tf;
        into.session_close = close;
        std::ifstream again(p, std::ios::binary);
        da::load_audit_csv(into, again, dir.filename().string() + "/" + p.filename().string());
    }
    for (auto& [tf, s] : by_tf) da::finish_audit_series(s);
    return by_tf;
}

[[nodiscard]] Instrument audit_instrument(const fs::path& root, const std::string& segment, const std::string& name,
                                          const std::vector<std::pair<std::string, fs::path>>& brokers) {
    Instrument ins;
    ins.segment = segment;
    ins.name = name;
    const fs::path base = root / segment / name;
    ins.provenance = read_provenance(base / "_manifest.json");
    const std::int64_t close = segment == "fut" || segment == "opt" ? da::kFnoSessionCloseMin
                                                                    : da::kSessionCloseMin;
    for (const int tf : kTimeframes) {
        const fs::path dir = base / da::audit_tf_name(tf);
        if (!fs::is_directory(dir)) continue;
        SeriesResult s;
        s.tf = tf;
        s.series = da::load_audit_dir(dir, tf, close);
        s.integrity = da::check_audit_series(s.series);
        ins.series.push_back(std::move(s));
    }
    // Every finer stored timeframe rebuilds every coarser one.
    std::vector<altair::data_audit::AuditBar> bars;
    std::vector<bool> complete;
    for (const auto& fine : ins.series) {
        for (const auto& coarse : ins.series) {
            if (fine.tf >= coarse.tf || fine.tf == da::kDailyTf) continue;
            da::split_buckets(da::aggregate_audit(fine.series, coarse.tf), bars, complete);
            CompareResult r;
            r.kind = "cross-timeframe";
            r.reference = da::audit_tf_name(coarse.tf);
            r.other = "rebuilt from " + da::audit_tf_name(fine.tf);
            r.cmp = da::compare_audit(r.reference + " vs " + r.other, coarse.series.bars, bars, complete);
            ins.compares.push_back(std::move(r));
        }
    }
    // Other sources kept in _superseded_* folders.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        const std::string n = e.path().filename().string();
        if (!e.is_directory() || n.rfind("_superseded", 0) != 0) continue;
        for (auto& [tf, other] : load_other_source(e.path(), close)) {
            const SeriesResult* mine = ins.tf(tf);
            if (mine == nullptr) continue;
            CompareResult r;
            r.kind = "other source";
            r.reference = da::audit_tf_name(tf);
            r.other = n + " " + da::audit_tf_name(tf);
            r.cmp = da::compare_audit(r.other, mine->series.bars, other.bars);
            ins.compares.push_back(std::move(r));
        }
    }
    // Broker candles.
    for (const auto& [broker, dir] : brokers) {
        for (const auto& mine : ins.series) {
            const fs::path bdir = dir / segment / name / da::audit_tf_name(mine.tf);
            if (!fs::is_directory(bdir)) continue;
            const da::AuditSeries theirs = da::load_audit_dir(bdir, mine.tf, close);
            CompareResult r;
            r.kind = "broker";
            r.reference = da::audit_tf_name(mine.tf);
            r.other = broker + " " + da::audit_tf_name(mine.tf);
            r.cmp = da::compare_audit(r.other, mine.series.bars, theirs.bars);
            ins.compares.push_back(std::move(r));
        }
    }
    return ins;
}

// ---- report ---------------------------------------------------------------------

XlsxCell s_(std::string t, bool bold = false) { return XlsxCell::str(std::move(t), bold); }
XlsxCell n_(double v) { return XlsxCell::num(v); }
XlsxCell z_(std::size_t v) { return XlsxCell::num(static_cast<double>(v)); }

XlsxRow integrity_header() {
    XlsxRow r;
    for (const char* h : {"Instrument", "TF", "Files", "Rows read", "Bars", "First", "Last", "Parse errors",
                          "Duplicate rows", "Conflicting rows", "Unsorted rows", "Bad OHLC", "Outside session",
                          "Off grid", "Weekend days", "Trading days", "Short days", "Missing bars",
                          "Seconds floored", "Status"})
        r.push_back(s_(h, true));
    return r;
}

XlsxRow integrity_row(const Instrument& ins, const SeriesResult& s) {
    const auto& a = s.series;
    const auto& i = s.integrity;
    const bool daily = s.tf == da::kDailyTf;
    return {s_(ins.id()), s_(da::audit_tf_name(s.tf)), z_(a.files.size()), z_(a.rows_read), z_(a.bars.size()),
            s_(a.bars.empty() ? "" : da::format_audit_time(a.bars.front().t, daily)),
            s_(a.bars.empty() ? "" : da::format_audit_time(a.bars.back().t, daily)),
            z_(a.parse_errors), z_(a.duplicates), z_(a.conflicts), z_(a.unsorted), z_(i.bad_ohlc),
            z_(i.outside_session), z_(i.off_grid), z_(i.weekend_days), z_(i.trading_days),
            z_(i.short_days), z_(i.missing_bars), z_(a.seconds_floored), s_(integrity_status(s), true)};
}

XlsxRow compare_header() {
    XlsxRow r;
    for (const char* h : {"Instrument", "Check", "Stored", "Against", "Overlap from", "Overlap to", "Compared bars",
                          "Not comparable", "Only in stored", "Only in other", "Exact", "Within 1 bp",
                          "Mismatch", "Match %", "Open mm", "High mm", "Low mm", "Close mm",
                          "Volume compared", "Volume equal %", "Median mm bp", "Worst bp", "Status"})
        r.push_back(s_(h, true));
    return r;
}

XlsxRow compare_row(const Instrument& ins, const CompareResult& r) {
    const auto& c = r.cmp;
    const bool daily = r.reference == "1d";
    return {s_(ins.id()), s_(r.kind), s_(r.reference), s_(r.other),
            s_(c.compared ? da::format_audit_time(c.overlap_from, daily) : ""),
            s_(c.compared ? da::format_audit_time(c.overlap_to, daily) : ""),
            z_(c.compared), z_(c.not_comparable), z_(c.only_reference), z_(c.only_other),
            z_(c.exact_bars), z_(c.rounding_bars), z_(c.mismatch_bars),
            n_(std::round(c.match_pct() * 100.0) / 100.0),
            z_(c.field_mismatch[0]), z_(c.field_mismatch[1]), z_(c.field_mismatch[2]), z_(c.field_mismatch[3]),
            z_(c.volume_compared),
            n_(c.volume_compared ? std::round(10000.0 * static_cast<double>(c.volume_equal)
                                              / static_cast<double>(c.volume_compared)) / 100.0
                                 : std::numeric_limits<double>::quiet_NaN()),
            n_(std::round(c.median_mismatch_bp * 100.0) / 100.0),
            n_(std::round(c.worst_bp * 100.0) / 100.0), s_(compare_status(r), true)};
}

XlsxSheet instrument_sheet(const Instrument& ins, std::size_t max_issues) {
    XlsxSheet sh;
    sh.name = ins.id().substr(0, 31);
    sh.widths = {26, 16, 22, 22, 22, 22, 14, 14, 14, 14, 12, 12, 12, 10, 10, 10, 10, 10, 14, 14, 10, 30};
    sh.rows.push_back({s_(ins.id() + " -- data audit", true)});
    for (const auto& [k, v] : ins.provenance) sh.rows.push_back({s_("manifest " + k), s_(v)});
    sh.rows.push_back({s_("Series integrity", true)});
    sh.rows.push_back(integrity_header());
    for (const auto& s : ins.series) sh.rows.push_back(integrity_row(ins, s));
    sh.rows.push_back({});
    sh.rows.push_back({s_("Comparisons (stored series vs rebuilt / other source / broker)", true)});
    sh.rows.push_back(compare_header());
    for (const auto& r : ins.compares) sh.rows.push_back(compare_row(ins, r));
    sh.rows.push_back({});
    sh.rows.push_back({s_("Issues (first " + std::to_string(max_issues)
                          + "; mismatches worst first within each check)", true)});
    XlsxRow head;
    for (const char* h : {"Kind", "Series / check", "Time", "Field", "Stored", "Other", "Diff", "bp", "Note"})
        head.push_back(s_(h, true));
    sh.rows.push_back(head);
    std::size_t shown = 0;
    std::size_t hidden = 0;
    const auto push = [&](XlsxRow row) {
        if (shown < max_issues) { sh.rows.push_back(std::move(row)); ++shown; }
        else ++hidden;
    };
    for (const auto& s : ins.series) {
        const std::string tf = da::audit_tf_name(s.tf);
        for (const auto& n : s.series.notes) push({s_("series"), s_(tf), s_(n.where), s_(""), s_(""), s_(""), s_(""), s_(""), s_(n.what)});
        for (const auto& n : s.integrity.notes) push({s_("integrity"), s_(tf), s_(n.where), s_(""), s_(""), s_(""), s_(""), s_(""), s_(n.what)});
    }
    for (const auto& r : ins.compares) {
        const bool daily = r.reference == "1d";
        const std::string label = r.reference + " vs " + r.other;
        for (const auto& d : r.cmp.diffs)
            push({s_("mismatch"), s_(label), s_(da::format_audit_time(d.t, daily)),
                  s_(da::kAuditFieldNames[static_cast<std::size_t>(d.field)]), n_(d.mine), n_(d.other),
                  n_(std::round((d.other - d.mine) * 10000.0) / 10000.0), n_(std::round(d.bp * 100.0) / 100.0),
                  s_("")});
        for (const auto t : r.cmp.missing_other)
            push({s_("missing"), s_(label), s_(da::format_audit_time(t, daily)), s_(""), s_(""), s_(""), s_(""),
                  s_(""), s_("in stored, not in " + r.other)});
        for (const auto t : r.cmp.missing_ref)
            push({s_("missing"), s_(label), s_(da::format_audit_time(t, daily)), s_(""), s_(""), s_(""), s_(""),
                  s_(""), s_("in " + r.other + ", not in stored")});
    }
    if (hidden > 0)   // RULE 11: truncated visibly -- the count of rows not shown is printed
        sh.rows.push_back({s_("... " + std::to_string(hidden) + " more issue rows not shown (--max-issues)", true)});
    sh.freeze_rows = 1;
    return sh;
}

void usage(const char* exe) {
    std::printf(
        "  Verify dataset/ across timeframes, other sources and broker candles.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--broker NAME=DIR]... [--max-issues N] [--no-merged]\n\n"
        "    --dataset DIR      default dataset\n"
        "    --out DIR          default data/verified (report + merged/ CSVs)\n"
        "    --broker NAME=DIR  broker candles laid out like dataset/, e.g.\n"
        "                       --broker FYERS=data/broker_audit/fyers --broker Kite=data/broker_audit/kite\n"
        "    --max-issues N     issue rows per instrument sheet (default 20000)\n"
        "    --no-merged        skip writing merged/ CSVs\n", exe);
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = "dataset";
    fs::path out = "data/verified";
    std::vector<std::pair<std::string, fs::path>> brokers;
    std::size_t max_issues = 20000;
    bool merged = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--no-merged") { merged = false; continue; }
        if (a == "--max-issues" && has) {
            const std::string_view v{argv[++i]};
            std::size_t n = 0;
            const auto r = std::from_chars(v.data(), v.data() + v.size(), n);
            if (r.ec != std::errc{} || n == 0 || n > 1'000'000) { usage(argv[0]); return 2; }
            max_issues = n;
            continue;
        }
        if (a == "--broker" && has) {
            const std::string_view v{argv[++i]};
            const std::size_t eq = v.find('=');
            if (eq == std::string_view::npos || eq == 0 || eq + 1 == v.size()) { usage(argv[0]); return 2; }
            brokers.emplace_back(std::string{v.substr(0, eq)}, fs::path{std::string{v.substr(eq + 1)}});
            continue;
        }
        usage(argv[0]);
        return 2;
    }
    if (!fs::is_directory(root)) {
        std::printf("  no dataset directory at %s\n", root.string().c_str());
        return 2;
    }
    for (const auto& [name, dir] : brokers)
        if (!fs::is_directory(dir)) std::printf("  note: broker folder %s (%s) does not exist; skipped\n",
                                                dir.string().c_str(), name.c_str());

    std::vector<Instrument> instruments;
    std::vector<fs::path> segments;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory() && e.path().filename().string()[0] != '.') segments.push_back(e.path());
    std::sort(segments.begin(), segments.end());
    for (const auto& seg : segments) {
        std::vector<fs::path> names;
        for (const auto& e : fs::directory_iterator(seg, ec))
            if (e.is_directory() && e.path().filename().string()[0] != '.') names.push_back(e.path());
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            std::printf("  auditing %s/%s ...\n", seg.filename().string().c_str(), n.filename().string().c_str());
            Instrument ins = audit_instrument(root, seg.filename().string(), n.filename().string(), brokers);
            if (!ins.series.empty()) instruments.push_back(std::move(ins));
        }
    }
    if (instruments.empty()) {
        std::printf("  no <segment>/<instrument>/<tf>/ series under %s\n", root.string().c_str());
        return 2;
    }

    fs::create_directories(out, ec);
    if (merged)
        for (const auto& ins : instruments)
            for (const auto& s : ins.series) write_merged(out / "merged", ins, s);

    // ---- summary: xlsx + text ---------------------------------------------------
    XlsxSheet summary;
    summary.name = "Summary";
    summary.widths = {26, 16, 22, 22, 22, 22, 14, 14, 14, 14, 12, 12, 12, 10, 10, 10, 10, 10, 14, 14, 10, 30};
    char stamp[32]{};
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    summary.rows.push_back({s_("Altair data audit", true)});
    summary.rows.push_back({s_("Generated"), s_(stamp), s_("Dataset"), s_(root.string())});
    summary.rows.push_back({s_("Tolerance"), s_("exact <= 0.005; within 1 bp = rounding; above 1 bp = mismatch")});
    summary.rows.push_back({s_("Note"), s_("1d close rebuilt from intraday differs from NSE's official index close "
                                           "(constituent closing prices, 30-min VWAP): reported as CLOSE ONLY.")});
    summary.rows.push_back({});
    summary.rows.push_back({s_("Series integrity", true)});
    summary.rows.push_back(integrity_header());
    std::ostringstream text;
    text << "Altair data audit  " << stamp << "  dataset=" << root.string() << "\n\n";
    for (const auto& ins : instruments)
        for (const auto& s : ins.series) {
            summary.rows.push_back(integrity_row(ins, s));
            text << "  " << ins.id() << " " << da::audit_tf_name(s.tf) << ": " << s.series.bars.size() << " bars, "
                 << s.integrity.short_days << " short days, " << s.series.conflicts << " conflicts, "
                 << s.integrity.bad_ohlc << " bad OHLC -> " << integrity_status(s) << "\n";
        }
    summary.rows.push_back({});
    summary.rows.push_back({s_("Comparisons", true)});
    summary.rows.push_back(compare_header());
    text << "\n";
    for (const auto& ins : instruments)
        for (const auto& r : ins.compares) {
            summary.rows.push_back(compare_row(ins, r));
            char pct[32];
            std::snprintf(pct, sizeof(pct), "%.2f%%", r.cmp.match_pct());
            text << "  " << ins.id() << " " << r.reference << " vs " << r.other << ": " << r.cmp.compared
                 << " compared, " << r.cmp.mismatch_bars << " mismatched, match " << pct << " -> "
                 << compare_status(r) << "\n";
        }
    summary.freeze_rows = 0;

    std::vector<XlsxSheet> sheets{summary};
    for (const auto& ins : instruments) sheets.push_back(instrument_sheet(ins, max_issues));
    const auto book = altair::xlsx::build_workbook(sheets);
    if (!book) {
        std::printf("  could not build the workbook\n");
        return 1;
    }
    {
        std::ofstream f(out / "data_audit.xlsx", std::ios::binary | std::ios::trunc);
        f.write(book->data(), static_cast<std::streamsize>(book->size()));
        if (!f) { std::printf("  could not write %s\n", (out / "data_audit.xlsx").string().c_str()); return 1; }
    }
    {
        std::ofstream f(out / "data_audit.txt", std::ios::binary | std::ios::trunc);
        f << text.str();
    }
    std::printf("%s\n  wrote %s\n", text.str().c_str(), (out / "data_audit.xlsx").string().c_str());
    if (merged) std::printf("  wrote merged series under %s\n", (out / "merged").string().c_str());
    return 0;
}
