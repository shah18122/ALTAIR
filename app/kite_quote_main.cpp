// app/kite_quote_main.cpp -- snapshot the READ-ONLY /quote endpoint.
//
// P29-01.
//
//     altair_kite_quote [--out PATH] [--keys "NSE:NIFTY 50,NSE:NIFTY BANK"] --go
//
// WHY THIS BINARY EXISTS AT ALL.
//
// `broker/kite_quote.hpp` has parsed a quote since P20-03 and has twenty-two
// green checks against it. Nothing has ever FETCHED one. The watchlist has
// therefore shown a last price from the replay tape and no bid, no ask, no
// depth and no spread -- the four things that make it a watchlist rather than a
// list.
//
// The parser was written first deliberately, and that was right: fetching is a
// socket and a header, parsing is where the bugs are. This is the other half.
//
// IT IS READ-ONLY, AND THAT IS STRUCTURAL.
//
// /quote is a GET. This program has no order vocabulary, builds no order body,
// and cannot place anything. `oms/` remains the only directory that can, and
// the UI reads the file this writes rather than calling anything itself.
//
// A QUOTE IS PERISHABLE AND THE FILE SAYS WHEN IT WAS TAKEN.
//
// `fetched_at_unix` is written beside the quotes for the same reason the
// account snapshot carries one: a bid with no timestamp gets believed. The
// panel shows the age before it shows a price.
//
// NOTHING SUCCEEDED MEANS NOTHING IS WRITTEN.
//
// An empty snapshot reads as an empty BOOK -- no bid, no ask, nothing quoted --
// which is a statement about the market rather than about the fetch. Same rule
// as the account snapshot: on total failure, write nothing and say so.

#include <broker/https_client.hpp>
#include <broker/kite_api.hpp>
#include <broker/kite_quote.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string env_or_empty(const char* name)
{
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string out{buf};
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string{v} : std::string{};
#endif
}

/// Read `access_token` out of the session file. Never printed.
bool read_access_token(const char* path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    const std::string body((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    const std::string key = "\"access_token\"";
    auto p = body.find(key);
    if (p == std::string::npos) { return false; }
    p = body.find('"', p + key.size());        // opening quote of the value
    if (p == std::string::npos) { return false; }
    const auto q = body.find('"', p + 1);
    if (q == std::string::npos) { return false; }
    out = body.substr(p + 1, q - p - 1);
    return !out.empty();
}

/// Split a comma-separated key list. Kite wants "EXCHANGE:TRADINGSYMBOL".
std::vector<std::string> split_keys(std::string_view s)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto c = s.find(',', start);
        const auto end = (c == std::string_view::npos) ? s.size() : c;
        std::string one{s.substr(start, end - start)};
        // Trim, because a list pasted from anywhere has spaces after commas.
        while (!one.empty() && one.front() == ' ') { one.erase(one.begin()); }
        while (!one.empty() && one.back() == ' ') { one.pop_back(); }
        if (!one.empty()) { out.push_back(one); }
        if (c == std::string_view::npos) { break; }
        start = end + 1;
    }
    return out;
}

/// JSON-escape for the key strings we echo back. A tradingsymbol has spaces
/// and colons and nothing else that needs escaping, but the quote and the
/// backslash are handled so a malformed --keys cannot produce malformed JSON.
std::string esc(std::string_view s)
{
    std::string o;
    o.reserve(s.size());
    for (const char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); }
        o.push_back(c);
    }
    return o;
}

void write_depth(std::ofstream& o, const char* name,
                 const altair::kite::KiteDepthLevel (&lv)[5])
{
    o << "      \"" << name << "\": [";
    bool first = true;
    for (const auto& l : lv) {
        // UNPOPULATED LEVELS ARE OMITTED, not written as zeros. A zeroed slot
        // rendered as a price is infinite liquidity at the best possible
        // price, which is the one thing a depth ladder must never draw.
        if (!l.populated) { continue; }
        if (!first) { o << ", "; }
        first = false;
        o << "{\"price_paise\": " << l.price.raw()
          << ", \"qty\": " << l.quantity
          << ", \"orders\": " << l.orders << "}";
    }
    o << "]";
}

void usage(const char* exe)
{
    std::printf(
        "  Snapshot the READ-ONLY /quote endpoint for the watchlist.\n\n"
        "    %s [--out PATH] [--keys LIST] [--go]\n\n"
        "    --out PATH   default data/kite_quotes.json\n"
        "    --keys LIST  comma-separated EXCHANGE:TRADINGSYMBOL.\n"
        "                 Default: the three the live grid carries.\n"
        "    --go         ACTUALLY CALL THE API. Without it this prints the\n"
        "                 request it would make and exits.\n\n"
        "  /quote is a GET. This program has no order vocabulary and cannot\n"
        "  place anything; oms/ remains the only directory that can.\n\n"
        "  Needs data/kite_session.json and ALTAIR_KITE_API_KEY. The access\n"
        "  token is read from the file and never printed.\n", exe);
}

} // namespace

int main(int argc, char** argv)
{
    using namespace altair;

    std::string out_path = "data/kite_quotes.json";
    std::string keys_arg = "NSE:NIFTY 50,NSE:NIFTY BANK,NSE:INDIA VIX";
    bool go = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (std::strcmp(argv[i], "--go") == 0) { go = true; }
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        }
        if (std::strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            keys_arg = argv[++i];
        }
    }

    const std::vector<std::string> keys = split_keys(keys_arg);
    if (keys.empty()) {
        std::printf("  --keys produced no instruments. Refusing.\n");
        return 1;
    }
    const std::string target = kite::quote_target(keys);

    std::printf("  Kite quote snapshot -> %s\n\n", out_path.c_str());
    for (const std::string& k : keys) {
        std::printf("    %s\n", k.c_str());
    }
    std::printf("\n    GET https://api.kite.trade%s\n", target.c_str());
    if (!go) {
        std::printf("\n  DRY RUN. Nothing was sent and nothing was written.\n"
                    "  Add --go to make this call for real.\n");
        return 0;
    }

    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    if (api_key.empty()) {
        std::printf("\n  ALTAIR_KITE_API_KEY is not set. Refusing.\n");
        return 2;
    }
    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("\n  no usable data/kite_session.json -- run "
                    "altair_kite_login first.\n");
        return 2;
    }
    const std::string auth = std::string("token ") + api_key + ":" + access;

    const auto r = https_get_auth("api.kite.trade", target, auth,
                                  kite::kVersion, std::chrono::seconds{20});
    if (!r) {
        std::printf("\n  TRANSPORT FAILED\n");
        return 1;
    }
    std::printf("\n    HTTP %ld  %zu bytes\n", r->status, r->body.size());
    if (r->status == 403) {
        std::printf("    token rejected -- Kite sessions are daily. Run "
                    "altair_kite_login.\n");
        return 1;
    }
    if (r->status != 200) { return 1; }

    // Parse each key SEPARATELY. One malformed instrument must not cost the
    // others: the watchlist can show four rows and one refusal, and that is
    // strictly better than five blanks.
    struct Got { std::string key; kite::KiteQuote q; bool ok = false;
                 const char* err = ""; };
    std::vector<Got> got;
    std::size_t ok_count = 0;
    for (const std::string& k : keys) {
        Got g;
        g.key = k;
        const auto p = kite::parse_quote(r->body, k);
        if (p) { g.q = *p; g.ok = true; ++ok_count; }
        else { g.err = kite::quote_error_text(p.error()); }
        std::printf("    %-24s %s\n", k.c_str(),
                    g.ok ? "ok" : g.err);
        got.push_back(std::move(g));
    }
    if (ok_count == 0) {
        std::printf("\n  Nothing parsed; not writing a snapshot.\n"
                    "  An EMPTY snapshot would read as an empty BOOK.\n");
        return 1;
    }

    const std::string tmp = out_path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            std::printf("\n  cannot write %s\n", tmp.c_str());
            return 1;
        }
        const auto now = std::time(nullptr);
        o << "{\n  \"fetched_at_unix\": " << static_cast<long long>(now)
          << ",\n  \"quotes\": {\n";
        bool first = true;
        for (const Got& g : got) {
            if (!first) { o << ",\n"; }
            first = false;
            o << "    \"" << esc(g.key) << "\": ";
            if (!g.ok) {
                // null, not {}. "not fetched" and "quoted nothing" are
                // different facts and the watchlist must be able to tell them
                // apart.
                o << "null";
                continue;
            }
            const kite::KiteQuote& q = g.q;
            o << "{\n      \"token\": " << q.instrument_token
              << ",\n      \"last_paise\": " << q.last_price.raw()
              << ",\n      \"volume\": " << q.volume
              << ",\n      \"buy_qty\": " << q.buy_quantity
              << ",\n      \"sell_qty\": " << q.sell_quantity
              << ",\n      \"open_paise\": " << q.open.raw()
              << ",\n      \"high_paise\": " << q.high.raw()
              << ",\n      \"low_paise\": " << q.low.raw()
              << ",\n      \"close_paise\": " << q.close.raw()
              << ",\n      \"lower_circuit_paise\": " << q.lower_circuit.raw()
              << ",\n      \"upper_circuit_paise\": " << q.upper_circuit.raw()
              << ",\n      \"has_touch\": " << (q.has_touch() ? "true" : "false")
              << ",\n      \"timestamp\": \"" << esc(q.timestamp) << "\",\n";
            write_depth(o, "buy", q.buy);
            o << ",\n";
            write_depth(o, "sell", q.sell);
            o << "\n    }";
        }
        o << "\n  }\n}\n";
        if (!o) {
            std::printf("\n  write failed partway through %s\n", tmp.c_str());
            return 1;
        }
    }
    std::remove(out_path.c_str());
    if (std::rename(tmp.c_str(), out_path.c_str()) != 0) {
        std::printf("\n  could not rename %s into place\n", tmp.c_str());
        return 1;
    }
    std::printf("\n  wrote %s  (%zu of %zu quoted)\n",
                out_path.c_str(), ok_count, keys.size());
    std::printf("  The panel shows this with its AGE. A bid with no\n"
                "  timestamp is worse than none, because it gets believed.\n");
    return 0;
}
