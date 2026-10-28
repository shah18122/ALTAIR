// app/fyers_chain_main.cpp -- save the day's NIFTY / BANKNIFTY option chains from FYERS.
//
// FYERS history refuses expired contracts, so a chain not saved before its
// expiry is gone for good. This saves, for each underlying, every listed
// future and the calls and puts of the nearest expiries (NIFTY: the three
// nearest, weeklies included, plus the monthly; BANKNIFTY: its monthlies) at
// the strikes nearest the spot, as one-minute bars and a close per day
// (app/fyers_chain.hpp says which bar is the close). The index levels and
// INDIA VIX go with them.
//
// The contract list is FYERS's own public F&O master
// (public.fyers.in/sym_details/NSE_FO.csv: no login), cached in
// data/live/fyers_masters/ and fetched again when older than 20 hours.
//
// Read-only: market data only, no orders. Dry run by default. A day already
// saved is kept unless --force. Exit 3 means the FYERS login has expired
// (ops/fyers_chain.ps1 logs it and skips); exit 1 means nothing was saved.
#include <app/dataset_merge.hpp>
#include <app/fyers_chain.hpp>
#include <app/fyers_env_session.hpp>
#include <broker/fyers_api.hpp>
#include <broker/fyers_historical.hpp>
#include <broker/https_client.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fc = altair::fyers_chain;

struct Session { std::string client; std::string access; };

[[nodiscard]] std::optional<std::string> json_string(std::string_view body, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    const auto name = body.find(needle);
    const auto colon = name == std::string_view::npos ? name : body.find(':', name + needle.size());
    const auto first = colon == std::string_view::npos ? colon : body.find('"', colon + 1);
    const auto last = first == std::string_view::npos ? first : body.find('"', first + 1);
    if (last == std::string_view::npos || last == first + 1) return std::nullopt;
    return std::string{body.substr(first + 1, last - first - 1)};
}

[[nodiscard]] std::optional<Session> read_session(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    constexpr std::size_t cap = 64 * 1024;
    std::string body(cap + 1, '\0');
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    const auto size = static_cast<std::size_t>(in.gcount());
    if (size == 0 || size > cap) return std::nullopt;
    body.resize(size);
    const auto client = json_string(body, "client_id");
    const auto access = json_string(body, "access_token");
    if (!client || !access || client->size() > altair::fyers::kClientIdMax
        || access->size() > altair::fyers::kAccessTokenMax) return std::nullopt;
    return Session{*client, *access};
}

[[nodiscard]] std::int64_t today_ist() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return fc::ist_day(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

/// NSE_FO.csv in `dir`, fetched when missing or older than 20 hours (only
/// with `fetch`). A download that does not work keeps the copy on disk.
[[nodiscard]] bool refresh_master(const fs::path& path, bool fetch) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const auto stamp = fs::last_write_time(path, ec);
    const bool fresh = !ec && fs::file_time_type::clock::now() - stamp < std::chrono::hours(20);
    if (fetch && !fresh) {
        const auto r = altair::https_get_auth("public.fyers.in", "/sym_details/NSE_FO.csv", "", "", std::chrono::seconds{60});
        if (r && r->status == 200 && r->body.size() > 1000) {
            const fs::path tmp = path.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(r->body.data(), static_cast<std::streamsize>(r->body.size()));
            }
            fs::rename(tmp, path, ec);
            std::printf("FYERS NSE_FO master downloaded (%zu bytes)\n", r->body.size());
        } else {
            std::printf("FYERS NSE_FO master: download did not work (%s)%s\n",
                        r ? ("HTTP " + std::to_string(r->status)).c_str() : "transport",
                        fs::exists(path, ec) ? "; using the copy on disk" : "");
        }
    }
    return fs::exists(path, ec);
}

enum class Fetch { Ok, Empty, LoginExpired, Failed };

void usage(const char* exe) {
    std::printf(
        "Save NIFTY / BANKNIFTY option chains from FYERS before they expire (dry run by default).\n\n"
        "  %s [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--root DIR] [--underlyings NIFTY,BANKNIFTY]\n"
        "     [--strikes N] [--nearest N] [--force] [--go]\n\n"
        "    --from, --to     IST days to save (default today); FYERS keeps a live contract's bars\n"
        "                     from its first traded day, so a missed day can be filled while it trades\n"
        "    --root DIR       the ALTAIR folder: data/live/fyers_masters, data/fyers_session.json,\n"
        "                     data/store/fyers_chain (default the source folder)\n"
        "    --strikes N      strikes each side of the money, per expiry (default 40)\n"
        "    --nearest N      NIFTY's nearest option expiries, weeklies included (default 3; BANKNIFTY 2)\n"
        "    --force          rewrite days already saved\n"
        "    --go             fetch and write; without it, print the plan only\n\n"
        "Exit 0 saved, 1 nothing saved, 2 bad arguments or no master, 3 FYERS login expired.\n",
        exe);
}

} // namespace

int main(int argc, char** argv) {
#ifdef ALTAIR_SOURCE_DIR
    fs::path root = ALTAIR_SOURCE_DIR;
#else
    fs::path root = ".";
#endif
    std::string from, to, unders = "NIFTY,BANKNIFTY";
    std::size_t strikes = 40, nearest = 3;
    bool go = false, force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--from" && has) { from = argv[++i]; }
        else if (a == "--to" && has) { to = argv[++i]; }
        else if (a == "--root" && has) { root = argv[++i]; }
        else if (a == "--underlyings" && has) { unders = argv[++i]; }
        else if (a == "--strikes" && has) { strikes = std::strtoul(argv[++i], nullptr, 10); }
        else if (a == "--nearest" && has) { nearest = std::strtoul(argv[++i], nullptr, 10); }
        else if (a == "--force") { force = true; }
        else if (a == "--go") { go = true; }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    const std::int64_t today = today_ist();
    if (from.empty()) { from = fc::day_text(today); }
    if (to.empty()) { to = fc::day_text(today); }
    const auto d_from = altair::bhavcopy::detail::date(from), d_to = altair::bhavcopy::detail::date(to);
    if (!d_from || !d_to || *d_to < *d_from || *d_to - *d_from > 366 || strikes == 0 || strikes > 200 || nearest == 0
        || nearest > 10) {
        usage(argv[0]);
        return 2;
    }
    std::set<std::string> underlyings;
    {
        std::stringstream ss(unders);
        std::string u;
        while (std::getline(ss, u, ',')) { if (u == "NIFTY" || u == "BANKNIFTY") { underlyings.insert(u); } }
    }
    if (underlyings.empty()) { usage(argv[0]); return 2; }

    const fs::path master = root / "data/live/fyers_masters/NSE_FO.csv";
    if (!refresh_master(master, go)) {
        std::printf("no FYERS NSE_FO master at %s%s\n", master.string().c_str(), go ? "" : " (run with --go to fetch it)");
        return 2;
    }
    std::vector<fc::Contract> listed;
    {
        std::ifstream in(master, std::ios::binary);
        std::size_t refused = 0;
        listed = fc::read_fo_master(in, underlyings, &refused);
        if (refused > 0) { std::printf("FYERS NSE_FO master: %zu row(s) the parser refused (not read)\n", refused); }
    }
    std::map<std::string, long> lot;
    for (const auto& c : listed) { if (c.expiry >= today) { lot.try_emplace(c.underlying, c.lot); } }
    for (const auto& [u, l] : lot) { std::printf("%s: lot %ld (FYERS master)\n", u.c_str(), l); }

    const std::map<std::string, std::string> spot_symbol{{"NIFTY", "NSE:NIFTY50-INDEX"}, {"BANKNIFTY", "NSE:NIFTYBANK-INDEX"}};
    const fs::path out_root = root / "data/store/fyers_chain";
    if (!go) {
        for (const auto& u : underlyings) {
            fc::Pick p;
            p.nearest = u == "NIFTY" ? nearest : 2;
            p.monthlies = u == "NIFTY" ? 1 : 2;
            p.strikes = strikes;
            std::set<std::int64_t> ks;
            for (const auto& c : listed) { if (c.underlying == u && c.kind != "FUT") { ks.insert(static_cast<std::int64_t>(c.strike)); } }
            const double mid = ks.empty() ? 0.0 : static_cast<double>(*std::next(ks.begin(), static_cast<long>(ks.size() / 2)));
            const auto chosen = fc::pick(listed, u, mid, *d_from, p);
            std::set<std::int64_t> exp;
            for (const auto& c : chosen) { if (c.kind != "FUT") { exp.insert(c.expiry); } }
            std::printf("%s: %zu contract(s) (strikes centred on the spot at run time); option expiries:", u.c_str(), chosen.size());
            for (const auto e : exp) { std::printf(" %s", fc::day_text(e).c_str()); }
            std::printf("\n");
        }
        std::printf("DRY RUN: %s to %s into %s. No request sent and no file written.\n", from.c_str(), to.c_str(),
                    out_root.string().c_str());
        return 0;
    }

    auto session = read_session(root / "data/fyers_session.json");
    if (!session) {
        if (const auto env = altair::fyers_env::from_environment()) { session = Session{env->client, env->access}; }
    }
    if (!session) {
        std::printf("no FYERS session: log in first (data/fyers_session.json), or set ALTAIR_FYERS_CLIENT_ID and ALTAIR_FYERS_ACCESS_TOKEN\n");
        return 3;
    }
    const auto authorization = altair::fyers::authorization_header(session->client.c_str(), session->access.c_str());
    if (!authorization) { return 3; }

    bool first_request = true;
    const auto fetch = [&](const std::string& symbol, std::vector<fc::Bar>& bars) -> Fetch {
        const auto chunks = altair::fyers_history::chunk_requests("1", from, to);
        if (!chunks) { return Fetch::Failed; }
        for (const auto& chunk : *chunks) {
            if (!first_request) { std::this_thread::sleep_for(std::chrono::milliseconds{350}); }   // 200 requests a minute
            first_request = false;
            const auto request = altair::fyers_history::uri(symbol, "1", chunk.from, chunk.to);
            if (!request) { return Fetch::Failed; }
            const auto r = altair::https_get_auth("api-t1.fyers.in", *request, *authorization, "", std::chrono::seconds{30});
            if (!r) { return Fetch::Failed; }
            if (r->status == 401 || r->status == 403) { return Fetch::LoginExpired; }
            if (r->status != 200) {
                const auto message = json_string(r->body, "message");
                std::printf("  %s: HTTP %u%s%s\n", symbol.c_str(), r->status, message ? ": " : "", message ? message->c_str() : "");
                return Fetch::Failed;
            }
            const auto parsed = altair::fyers_history::parse(r->body);
            if (!parsed) { return Fetch::Failed; }
            for (const auto& c : *parsed) {
                if (!bars.empty() && c.ts_ns <= bars.back().ts_ns) { continue; }
                bars.push_back({c.ts_ns, c.open, c.high, c.low, c.close, c.volume});
            }
        }
        return bars.empty() ? Fetch::Empty : Fetch::Ok;
    };

    // VIX once, filed with NIFTY.
    std::vector<fc::Bar> vix;
    if (underlyings.contains("NIFTY")) {
        const auto f = fetch("NSE:INDIAVIX-INDEX", vix);
        if (f == Fetch::LoginExpired) { std::printf("FYERS login expired: log in again, then rerun\n"); return 3; }
    }

    std::size_t saved_days = 0, total_bars = 0;
    for (const auto& u : underlyings) {
        std::vector<fc::Bar> spot;
        const auto sf = fetch(spot_symbol.at(u), spot);
        if (sf == Fetch::LoginExpired) { std::printf("FYERS login expired: log in again, then rerun\n"); return 3; }
        if (sf != Fetch::Ok) { std::printf("%s: no index bars from FYERS for %s to %s; skipped\n", u.c_str(), from.c_str(), to.c_str()); continue; }
        fc::Pick p;
        p.nearest = u == "NIFTY" ? nearest : 2;
        p.monthlies = u == "NIFTY" ? 1 : 2;
        p.strikes = strikes;
        std::vector<fc::Contract> chosen = fc::pick(listed, u, spot.back().close, *d_from, p);
        fc::Contract sc;
        sc.ticker = spot_symbol.at(u);
        sc.underlying = u;
        sc.kind = "SPOT";
        std::vector<std::pair<fc::Contract, std::vector<fc::Bar>>> got;
        got.push_back({sc, std::move(spot)});
        if (u == "NIFTY" && !vix.empty()) {
            fc::Contract vc;
            vc.ticker = "NSE:INDIAVIX-INDEX";
            vc.underlying = u;
            vc.kind = "VIX";
            got.push_back({vc, vix});
        }
        std::printf("%s: fetching %zu contract(s), ATM near %.2f\n", u.c_str(), chosen.size(), got.front().second.back().close);
        std::fflush(stdout);
        std::size_t empty = 0, failed = 0;
        for (const auto& c : chosen) {
            std::vector<fc::Bar> bars;
            const auto f = fetch(c.ticker, bars);
            if (f == Fetch::LoginExpired) { std::printf("FYERS login expired mid-run: nothing for %s written\n", u.c_str()); return 3; }
            if (f == Fetch::Empty) { ++empty; continue; }
            if (f == Fetch::Failed) { ++failed; continue; }
            got.push_back({c, std::move(bars)});
        }
        if (empty + failed > 0) { std::printf("  %s: %zu contract(s) had no bars, %zu failed\n", u.c_str(), empty, failed); }

        // Split by IST day and write.
        std::map<std::int64_t, std::pair<std::string, std::string>> files;   // day -> closes, bars
        for (const auto& [c, bars] : got) {
            std::map<std::int64_t, std::vector<fc::Bar>> by_day;
            for (const auto& b : bars) { by_day[fc::ist_day(b.ts_ns)].push_back(b); }
            for (const auto& [day, db] : by_day) {
                if (day < *d_from || day > *d_to) { continue; }
                auto& [closes, minute] = files[day];
                if (closes.empty()) { closes = fc::closes_header(); minute = "ticker,time,open,high,low,close,volume\n"; }
                if (const auto dc = fc::day_close(db)) { closes += fc::close_row(c, *dc); }
                for (const auto& b : db) {
                    char line[256];
                    std::snprintf(line, sizeof line, "%s,%s,%.2f,%.2f,%.2f,%.2f,%.0f\n", c.ticker.c_str(),
                                  altair::format_ist(b.ts_ns).c_str(), b.open, b.high, b.low, b.close, b.volume);
                    minute += line;
                    ++total_bars;
                }
            }
        }
        for (const auto& [day, text] : files) {
            const fs::path dir = out_root / fc::day_text(day);
            const fs::path closes = dir / (u + ".csv"), minute = dir / (u + "_1m.csv");
            std::error_code ec;
            if (!force && fs::exists(closes, ec)) {
                std::printf("  %s %s: already saved, kept (--force rewrites)\n", u.c_str(), fc::day_text(day).c_str());
                continue;
            }
            fs::create_directories(dir, ec);
            if (!altair::dataset::replace_file_checked(closes, text.first)
                || !altair::dataset::replace_file_checked(minute, text.second)) {
                std::printf("  %s %s: write failed\n", u.c_str(), fc::day_text(day).c_str());
                continue;
            }
            std::size_t rows = 0;
            for (const char ch : text.first) { rows += ch == '\n'; }
            std::printf("  %s %s: %zu close(s) -> %s\n", u.c_str(), fc::day_text(day).c_str(), rows - 1, closes.string().c_str());
            ++saved_days;
        }
    }
    if (saved_days > 0) {
        std::error_code ec;
        fs::create_directories(out_root, ec);
        std::ofstream(out_root / "SOURCE.txt") << "FYERS /data/history one-minute bars, saved by altair_fyers_chain\n";
    }
    if (total_bars == 0) {
        std::printf("WARNING: FYERS returned no bars for %s to %s; nothing saved\n", from.c_str(), to.c_str());
        return 1;
    }
    std::printf("saved %zu underlying-day(s), %zu one-minute bar(s)\n", saved_days, total_bars);
    return saved_days > 0 ? 0 : 1;
}
