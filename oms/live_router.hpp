// oms/live_router.hpp -- the order router: a Terminal request becomes a FYERS
// order. THE ONLY PATH FROM A BUTTON TO REAL MONEY.
//
// The desktop appends a request to data/order_intents.jsonl
// (desktop/order_ticket.hpp). This drains it (oms/intent_queue.hpp), checks it
// against the arm file and the limits in it, asks FYERS for the price and the
// day's P&L, runs the dispatch gate (oms/broker_dispatch_gate.hpp), rechecks
// the gate immediately before sending, sends it, and then follows the order in
// the FYERS order book until it is done.
//
// OFF UNLESS ARMED. data/live_trading.json, written by the Terminal's LIVE
// switch after a typed confirmation, says "armed", until when (15:30 IST that
// day at the latest) and the limits. A missing, unreadable, expired or
// out-of-bounds arm file refuses every request. data/kill_request.json
// (desktop/kill_switch.hpp) refuses every request and cancels every open order
// this router placed; so does a file that cannot be checked.
//
// AT MOST ONCE. Nothing is retried. A send whose outcome is unknown (no reply,
// no order id) is looked for in the order book by its tag; if it is not there
// after a few looks it is marked NOT PLACED -- and never sent again.
//
// No transport here: RouterTransport is two functions that the router main
// (oms/order_router_main.cpp) fills with broker/https_client.hpp and the test
// fills with a fake exchange.
#pragma once

#include <oms/broker_dispatch_gate.hpp>
#include <oms/fyers_adapter.hpp>
#include <oms/intent_queue.hpp>
#include <oms/order_intent.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace altair::oms {

// ---- a small JSON reader: FYERS replies and the router's own files ------------

/// Longest document read. A busy day's FYERS order book is well under 1 MB.
inline constexpr std::size_t kRouterJsonMaxBytes = 8u * 1024u * 1024u;
/// Deepest nesting read. FYERS replies are three levels deep.
inline constexpr int kRouterJsonMaxDepth = 32;

struct RouterJson {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool flag = false;
    double number = 0.0;
    std::string text;
    std::vector<RouterJson> items;
    std::vector<std::pair<std::string, RouterJson>> fields;

    [[nodiscard]] const RouterJson* get(std::string_view key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [k, v] : fields)
            if (k == key) return &v;
        return nullptr;
    }
    [[nodiscard]] bool has_num(std::string_view key) const {
        const RouterJson* v = get(key);
        return v != nullptr && v->kind == Kind::Number;
    }
    /// A number, or a string holding one; `fallback` otherwise.
    [[nodiscard]] double num(std::string_view key, double fallback = 0.0) const {
        const RouterJson* v = get(key);
        if (v == nullptr) return fallback;
        if (v->kind == Kind::Number) return v->number;
        if (v->kind == Kind::String && !v->text.empty()) {
            char* end = nullptr;
            const double d = std::strtod(v->text.c_str(), &end);
            if (end != nullptr && *end == '\0' && std::isfinite(d)) return d;
        }
        return fallback;
    }
    /// A string; an integral number as its digits (FYERS sends some ids as numbers).
    [[nodiscard]] std::string str(std::string_view key) const {
        const RouterJson* v = get(key);
        if (v == nullptr) return {};
        if (v->kind == Kind::String) return v->text;
        if (v->kind == Kind::Number && std::fabs(v->number) < 9.0e15 && v->number == std::floor(v->number)) {
            char b[32];
            std::snprintf(b, sizeof b, "%.0f", v->number);
            return b;
        }
        return {};
    }
    [[nodiscard]] bool truth(std::string_view key) const {
        const RouterJson* v = get(key);
        return v != nullptr && v->kind == Kind::Bool && v->flag;
    }
};

namespace router_detail {

class JsonReader {
public:
    explicit JsonReader(std::string_view s) : s_(s) {}
    [[nodiscard]] std::optional<RouterJson> document() {
        RouterJson v;
        if (!value(v, 0)) return std::nullopt;
        ws();
        if (i_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool word(std::string_view w) {
        if (s_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    bool more() {
        ws();
        if (i_ < s_.size() && s_[i_] == ',') { ++i_; return true; }
        return false;
    }
    bool value(RouterJson& out, int depth) {
        // RULE 11: nesting is bounded; a deeper document is refused, not recursed into.
        if (depth > kRouterJsonMaxDepth) return false;
        ws();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            out.kind = RouterJson::Kind::Object;
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                RouterJson v;
                if (!value(v, depth + 1)) return false;
                out.fields.emplace_back(std::move(key), std::move(v));
                if (more()) continue;
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.kind = RouterJson::Kind::Array;
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                RouterJson v;
                if (!value(v, depth + 1)) return false;
                out.items.push_back(std::move(v));
                if (more()) continue;
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { out.kind = RouterJson::Kind::String; return string(out.text); }
        if (word("true")) { out.kind = RouterJson::Kind::Bool; out.flag = true; return true; }
        if (word("false")) { out.kind = RouterJson::Kind::Bool; out.flag = false; return true; }
        if (word("null")) { out.kind = RouterJson::Kind::Null; return true; }
        const std::size_t start = i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '-' || s_[i_] == '+'
                                  || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
            ++i_;
        if (i_ == start) return false;
        const std::string n(s_.substr(start, i_ - start));
        char* end = nullptr;
        out.number = std::strtod(n.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(out.number)) return false;
        out.kind = RouterJson::Kind::Number;
        return true;
    }
    bool string(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i_ + 4 > s_.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s_[i_ + static_cast<std::size_t>(k)];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                i_ += 4;
                // Ids and messages are ASCII; a surrogate half is kept as U+FFFD.
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

[[nodiscard]] inline std::string quoted(std::string_view s) {
    std::string out;
    fyers_detail::json_string(out, s);
    return out;
}

[[nodiscard]] inline std::string file_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

[[nodiscard]] inline std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

} // namespace router_detail

[[nodiscard]] inline std::optional<RouterJson> parse_router_json(std::string_view s) {
    // RULE 11: a document longer than the bound is refused whole.
    if (s.size() > kRouterJsonMaxBytes) return std::nullopt;
    return router_detail::JsonReader(s).document();
}

// ---- the arm file --------------------------------------------------------------

inline constexpr const char* kRouterArmFile = "data/live_trading.json";
inline constexpr const char* kRouterKillFile = "data/kill_request.json";
inline constexpr const char* kRouterDir = "data/live_orders";

// Ceilings an arm file may not exceed. RULE 11: beyond them the file is
// REFUSED, never clamped -- a limit somebody typed as 500 is not quietly 50.
inline constexpr std::int64_t kRouterCeilingLots = 50;
inline constexpr std::int64_t kRouterCeilingOrderValuePaise = 1'00'00'000LL * 100;   // Rs 1 crore
inline constexpr std::int64_t kRouterCeilingOrdersPerDay = 200;
inline constexpr std::int64_t kRouterCeilingOpenOrders = 50;
inline constexpr std::int64_t kRouterCeilingDailyLossPaise = 10'00'000LL * 100;      // Rs 10 lakh
inline constexpr double kRouterCeilingBandPct = 20.0;
/// An arm lasts at most this long: one session, never overnight.
inline constexpr std::int64_t kRouterMaxArmSeconds = 8 * 3600;

struct RouterArm {
    bool armed = false;
    std::string by;
    std::int64_t armed_unix = 0;
    std::int64_t expires_unix = 0;
    std::int64_t max_lots = 1;                                 ///< per order
    std::int64_t max_order_value_paise = 25'00'000LL * 100;    ///< price x quantity; Rs 25 lakh is one index-future lot
    std::int64_t max_orders_per_day = 20;
    std::int64_t max_open_orders = 5;
    std::int64_t max_daily_loss_paise = 5'000LL * 100;         ///< FYERS positions' P&L for the day
    double price_band_pct = 3.0;                               ///< a limit this far from the last price is refused
};

enum class RouterArmVerdict : std::uint8_t { Unreadable = 0, Absent, Disarmed, Expired, OutOfBounds, Armed };

struct RouterArmRead {
    RouterArmVerdict verdict = RouterArmVerdict::Unreadable;
    RouterArm arm;
    std::string why;
};

/// Read the arm file's text. Every limit must be present and inside its
/// ceiling; the arm must be current and no longer than one session.
[[nodiscard]] inline RouterArmRead read_router_arm(std::string_view text, std::int64_t now_unix) {
    RouterArmRead r;
    const auto j = parse_router_json(text);
    if (!j || j->kind != RouterJson::Kind::Object) {
        r.why = "the arm file does not parse: live trading stays off";
        return r;
    }
    RouterArm& a = r.arm;
    a.armed = j->truth("armed");
    a.by = j->str("by");
    a.armed_unix = static_cast<std::int64_t>(j->num("armed_unix", 0.0));
    a.expires_unix = static_cast<std::int64_t>(j->num("expires_unix", 0.0));
    if (!a.armed) {
        r.verdict = RouterArmVerdict::Disarmed;
        r.why = "live trading is off (the Terminal's LIVE switch)";
        return r;
    }
    const double lots = j->num("max_lots", -1.0), value = j->num("max_order_value", -1.0),
                 orders = j->num("max_orders_per_day", -1.0), open = j->num("max_open_orders", -1.0),
                 loss = j->num("max_daily_loss", -1.0), band = j->num("price_band_pct", -1.0);
    if (lots < 1.0 || lots > static_cast<double>(kRouterCeilingLots) || lots != std::floor(lots)
        || value <= 0.0 || value * 100.0 > static_cast<double>(kRouterCeilingOrderValuePaise)
        || orders < 1.0 || orders > static_cast<double>(kRouterCeilingOrdersPerDay)
        || open < 1.0 || open > static_cast<double>(kRouterCeilingOpenOrders)
        || loss <= 0.0 || loss * 100.0 > static_cast<double>(kRouterCeilingDailyLossPaise)
        || band <= 0.0 || band > kRouterCeilingBandPct) {
        r.verdict = RouterArmVerdict::OutOfBounds;
        r.why = "the arm file's limits are missing or beyond the router's ceilings: live trading stays off";
        return r;
    }
    a.max_lots = static_cast<std::int64_t>(lots);
    a.max_order_value_paise = std::llround(value * 100.0);
    a.max_orders_per_day = static_cast<std::int64_t>(orders);
    a.max_open_orders = static_cast<std::int64_t>(open);
    a.max_daily_loss_paise = std::llround(loss * 100.0);
    a.price_band_pct = band;
    if (a.expires_unix <= now_unix || a.armed_unix <= 0 || a.armed_unix > now_unix + 60
        || a.expires_unix - a.armed_unix > kRouterMaxArmSeconds) {
        r.verdict = RouterArmVerdict::Expired;
        r.why = "the arm has expired: switch LIVE on again in the Terminal";
        return r;
    }
    r.verdict = RouterArmVerdict::Armed;
    return r;
}

// ---- instruments, time, limits ---------------------------------------------------

struct RouterInstrument {
    std::uint32_t token = 0;
    std::string symbol;            ///< the tradingsymbol the Terminal shows
    std::string fyers;             ///< NSE:SBIN-EQ, NSE:NIFTY26OCTFUT
    std::string exchange;          ///< NSE or BSE (equity), NFO (F&O)
    std::int64_t lot = 0;
    std::int64_t tick_paise = 0;
};

struct RouterClock {
    std::int64_t day = 0;            ///< IST days since 1970-01-01
    std::int64_t second_of_day = 0;  ///< IST
    int weekday = 0;                 ///< 0 = Monday
};

[[nodiscard]] inline RouterClock router_ist(std::int64_t now_ns) noexcept {
    const std::int64_t s = router_detail::floor_div(now_ns, 1'000'000'000LL) + 19'800;
    const std::int64_t day = router_detail::floor_div(s, 86'400);
    // 1970-01-01 was a Thursday.
    return RouterClock{day, s - day * 86'400, static_cast<int>(((day % 7) + 7 + 3) % 7)};
}

/// The normal session, 09:15 to 15:30 IST on a weekday. Holidays are not
/// known here; FYERS refuses an order on one.
[[nodiscard]] inline bool router_market_open(std::int64_t now_ns) noexcept {
    const RouterClock c = router_ist(now_ns);
    return c.weekday < 5 && c.second_of_day >= 9 * 3600 + 15 * 60 && c.second_of_day < 15 * 3600 + 30 * 60;
}

/// What the router knows about today when a request arrives.
struct RouterBook {
    std::int64_t orders_today = 0;               ///< sent today, whatever became of them
    std::int64_t open_orders = 0;                ///< sent and not yet final
    std::optional<std::int64_t> day_pnl_paise;   ///< FYERS positions; nullopt = not read
};

/// One request, checked and translated: the exact body that will be sent.
struct RouterPlan {
    std::string fyers_symbol;
    std::string product;          ///< FYERS: CNC, INTRADAY, MARGIN
    std::string validity;
    std::string tag;
    std::int64_t qty = 0;
    std::int64_t limit_paise = 0;
    std::int64_t value_paise = 0;
    std::string body;
};

/// The order's tag at FYERS: letters and digits from the request's id, so an
/// uncertain send can be found again in the order book.
[[nodiscard]] inline std::string router_order_tag(std::string_view intent_id) {
    char b[24];
    std::snprintf(b, sizeof b, "AL%016llX",
                  static_cast<unsigned long long>(detail::fnv1a(intent_id.data(), intent_id.size())));
    return b;
}

/// Every check a request must pass before it may be sent. The message of a
/// refusal is shown to the person who pressed the button, so it says which
/// limit and by how much.
[[nodiscard]] inline std::expected<RouterPlan, std::string>
plan_router_order(const OrderIntent& in, const RouterInstrument* inst, const RouterArm& arm,
                  const RouterBook& book, std::optional<std::int64_t> ltp_paise, std::int64_t now_ns) {
    const auto rs = [](std::int64_t paise) {
        char b[48];
        std::snprintf(b, sizeof b, "Rs %lld.%02lld", static_cast<long long>(paise / 100),
                      static_cast<long long>((paise < 0 ? -paise : paise) % 100));
        return std::string(b);
    };
    if (inst == nullptr) return std::unexpected("token " + std::to_string(in.token) + " is not in today's universe");
    if (inst->exchange != in.exchange)
        return std::unexpected(in.symbol + " is on " + inst->exchange + ", the request says " + in.exchange);
    if (inst->symbol != in.symbol)
        return std::unexpected("token " + std::to_string(in.token) + " is " + inst->symbol + ", the request says " + in.symbol);
    if (inst->lot <= 0 || inst->tick_paise <= 0 || inst->fyers.empty())
        return std::unexpected(in.symbol + " has no lot size, tick or FYERS symbol in the universe");
    if (!router_market_open(now_ns)) return std::unexpected("the market is closed (orders go 09:15 to 15:30 IST, weekdays)");
    if (in.lots > arm.max_lots)
        return std::unexpected(std::to_string(in.lots) + " lots is over the limit of " + std::to_string(arm.max_lots) + " per order");

    RouterPlan p;
    p.fyers_symbol = inst->fyers;
    p.validity = in.validity;
    p.tag = router_order_tag(in.id);
    const bool equity = inst->exchange == "NSE" || inst->exchange == "BSE";
    if (in.product == "MIS") p.product = "INTRADAY";
    else if (in.product == "CNC" && equity) p.product = "CNC";
    else if (in.product == "NRML" && !equity) p.product = "MARGIN";
    else return std::unexpected(in.product + (equity ? " is not an equity product (CNC or MIS)" : " is not an F&O product (NRML or MIS)"));

    // RULE 11: lots and the lot size are both bounded, but their product is checked anyway.
    if (inst->lot > std::numeric_limits<std::int64_t>::max() / in.lots)
        return std::unexpected("the quantity overflows");
    p.qty = in.lots * inst->lot;

    if (!ltp_paise || *ltp_paise <= 0) return std::unexpected("FYERS gave no last price for " + inst->fyers + ": not sent");
    const std::int64_t ltp = *ltp_paise;
    if (in.order_type == IntentType::Limit) {
        if (in.limit_paise % inst->tick_paise != 0)
            return std::unexpected("the limit " + rs(in.limit_paise) + " is not on the " + rs(inst->tick_paise) + " tick");
        const double off = std::fabs(static_cast<double>(in.limit_paise - ltp)) / static_cast<double>(ltp) * 100.0;
        if (off > arm.price_band_pct) {
            char b[160];
            std::snprintf(b, sizeof b, "the limit is %.2f%% from the last price %s (band %.2f%%)", off, rs(ltp).c_str(),
                          arm.price_band_pct);
            return std::unexpected(std::string(b));
        }
        p.limit_paise = in.limit_paise;
    }
    const std::int64_t price = in.order_type == IntentType::Limit ? in.limit_paise : ltp;
    if (price > std::numeric_limits<std::int64_t>::max() / p.qty) return std::unexpected("the order value overflows");
    p.value_paise = price * p.qty;
    if (p.value_paise > arm.max_order_value_paise)
        return std::unexpected("the order is worth " + rs(p.value_paise) + ", over the limit of " + rs(arm.max_order_value_paise));
    if (book.orders_today >= arm.max_orders_per_day)
        return std::unexpected(std::to_string(book.orders_today) + " orders today: the limit is " + std::to_string(arm.max_orders_per_day));
    if (book.open_orders >= arm.max_open_orders)
        return std::unexpected(std::to_string(book.open_orders) + " orders are open: the limit is " + std::to_string(arm.max_open_orders));
    if (!book.day_pnl_paise) return std::unexpected("today's P&L could not be read from FYERS positions: not sent");
    if (*book.day_pnl_paise <= -arm.max_daily_loss_paise)
        return std::unexpected("today's P&L is " + rs(*book.day_pnl_paise) + ": the loss limit of " + rs(arm.max_daily_loss_paise)
                               + " is reached");

    const FyersOrderIntent fo{p.fyers_symbol, Qty{p.qty}, in.side == IntentSide::Buy ? Side::Buy : Side::Sell,
                              in.order_type == IntentType::Limit ? FyersOrderType::Limit : FyersOrderType::Market,
                              p.product, Price{p.limit_paise}, Price{0}, p.validity, p.tag, false};
    auto body = build_fyers_place_order(fo);
    if (!body) return std::unexpected("the order could not be written in FYERS's terms");
    p.body = std::move(*body);
    return p;
}

// ---- the dispatch gate, from what the router has seen ------------------------------

/// How long one authenticated reply, one positions read and one risk check count.
inline constexpr std::int64_t kRouterEvidenceNs = 60'000'000'000LL;
inline constexpr std::int64_t kRouterRiskNs = 10'000'000'000LL;

struct RouterEvidence {
    bool armed = false;
    bool killed = true;
    bool transport = false;
    std::uint64_t arm_revision = 0;          ///< changes whenever the arm file does
    std::uint64_t session_generation = 0;    ///< changes whenever the FYERS login does
    std::int64_t auth_ok_ns = 0;             ///< last reply the session was accepted on
    std::int64_t account_ok_ns = 0;          ///< last positions read
    bool risk_passed = false;
    std::int64_t risk_ns = 0;
};

struct RouterGate {
    broker_view::Routes routes;
    broker_view::BrokerEvidence evidence;
    broker_view::OrderGate gate;
};

[[nodiscard]] inline RouterGate router_gate(const RouterEvidence& e) {
    using namespace broker_view;
    RouterGate g;
    g.routes.data_primary = BrokerId::Fyers;
    g.routes.order_primary = BrokerId::Fyers;
    g.routes.mode = e.armed ? TradingMode::LiveArmed : TradingMode::LiveDisabled;
    g.routes.revision = e.arm_revision;
    const SessionKey s{BrokerId::Fyers, 1, e.session_generation};
    g.evidence.session = s;
    g.evidence.auth = e.auth_ok_ns > 0 ? AuthStatus::Authenticated : AuthStatus::Unverified;
    g.evidence.authentication = EvidenceWindow{Timestamp{e.auth_ok_ns}, Timestamp{e.auth_ok_ns + kRouterEvidenceNs}};
    g.evidence.account_session = s;
    g.evidence.account_snapshot = EvidenceWindow{Timestamp{e.account_ok_ns}, Timestamp{e.account_ok_ns + kRouterEvidenceNs}};
    g.gate.session = s;
    g.gate.route_revision = e.arm_revision;
    g.gate.risk_check = EvidenceWindow{Timestamp{e.risk_ns}, Timestamp{e.risk_ns + kRouterRiskNs}};
    g.gate.risk_passed = e.risk_passed;
    g.gate.transport_permitted = e.transport;
    g.gate.kill_switch_active = e.killed;
    return g;
}

// ---- orders and what FYERS says about them -------------------------------------------

enum class RouterOrderStatus : std::uint8_t {
    Refused = 0, DryRun, Uncertain, Pending, Open, Filled, Cancelled, Rejected, Expired, NotPlaced
};

[[nodiscard]] inline const char* router_status_text(RouterOrderStatus s) noexcept {
    switch (s) {
    case RouterOrderStatus::Refused: return "REFUSED";
    case RouterOrderStatus::DryRun: return "DRY RUN";
    case RouterOrderStatus::Uncertain: return "UNCERTAIN";
    case RouterOrderStatus::Pending: return "PENDING";
    case RouterOrderStatus::Open: return "OPEN";
    case RouterOrderStatus::Filled: return "FILLED";
    case RouterOrderStatus::Cancelled: return "CANCELLED";
    case RouterOrderStatus::Rejected: return "REJECTED";
    case RouterOrderStatus::Expired: return "EXPIRED";
    case RouterOrderStatus::NotPlaced: return "NOT PLACED";
    }
    return "?";
}

[[nodiscard]] inline std::optional<RouterOrderStatus> router_status_from_text(std::string_view t) noexcept {
    for (int k = 0; k <= static_cast<int>(RouterOrderStatus::NotPlaced); ++k) {
        const auto s = static_cast<RouterOrderStatus>(k);
        if (t == router_status_text(s)) return s;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool router_final(RouterOrderStatus s) noexcept {
    return s != RouterOrderStatus::Uncertain && s != RouterOrderStatus::Pending && s != RouterOrderStatus::Open;
}

/// Order-book reads that may miss an uncertain send before it is NOT PLACED.
inline constexpr int kRouterUncertainLooks = 3;

struct RouterOrder {
    std::string intent_id, tag, fyers_id;
    std::string symbol, fyers_symbol, side, type, product, by;
    std::int64_t lots = 0, qty = 0, limit_paise = 0, filled = 0, avg_paise = 0;
    RouterOrderStatus status = RouterOrderStatus::Refused;
    std::string message;
    std::int64_t at_ns = 0, updated_ns = 0;
    int looks = 0;
    bool cancel_sent = false;
};

/// Fold a FYERS order-book reply into the rows: by the FYERS id, or -- for a
/// send whose outcome was unknown -- by the tag. An uncertain send not found
/// after kRouterUncertainLooks good reads was not placed. Returns rows changed.
inline std::size_t apply_router_book(std::vector<RouterOrder>& rows, const RouterJson& reply, std::int64_t now_ns) {
    if (reply.str("s") != "ok") return 0;
    const RouterJson* book = reply.get("orderBook");
    static const RouterJson kEmpty = [] {
        RouterJson e;
        e.kind = RouterJson::Kind::Array;
        return e;
    }();
    if (book == nullptr || book->kind != RouterJson::Kind::Array) book = &kEmpty;
    std::size_t changed = 0;
    for (RouterOrder& r : rows) {
        if (r.status == RouterOrderStatus::Refused || r.status == RouterOrderStatus::DryRun
            || r.status == RouterOrderStatus::NotPlaced)
            continue;
        const RouterJson* hit = nullptr;
        for (const RouterJson& e : book->items) {
            if (!r.fyers_id.empty() ? e.str("id") == r.fyers_id : (!r.tag.empty() && e.str("orderTag") == r.tag)) {
                hit = &e;
                break;
            }
        }
        if (hit == nullptr) {
            if (r.status == RouterOrderStatus::Uncertain && ++r.looks >= kRouterUncertainLooks) {
                r.status = RouterOrderStatus::NotPlaced;
                r.message = "not in the FYERS order book after " + std::to_string(r.looks)
                          + " looks: it was not placed, and is never re-sent";
                r.updated_ns = now_ns;
                ++changed;
            }
            continue;
        }
        RouterOrder before = r;
        if (r.fyers_id.empty()) r.fyers_id = hit->str("id");
        const auto st = map_fyers_status(static_cast<int>(hit->num("status", -1.0)));
        if (st) {
            switch (*st) {
            case OrderState::PendingNew: r.status = RouterOrderStatus::Pending; break;
            case OrderState::Open: r.status = RouterOrderStatus::Open; break;
            case OrderState::Filled: r.status = RouterOrderStatus::Filled; break;
            case OrderState::Cancelled: r.status = RouterOrderStatus::Cancelled; break;
            case OrderState::Rejected: r.status = RouterOrderStatus::Rejected; break;
            case OrderState::Expired: r.status = RouterOrderStatus::Expired; break;
            default: break;
            }
        }
        // Cumulative quantities only rise (oms/order_state.hpp): a stale read never undoes a fill.
        const auto filled = static_cast<std::int64_t>(hit->num("filledQty", 0.0));
        if (filled > r.filled) r.filled = filled;
        const double avg = hit->num("tradedPrice", 0.0);
        if (avg > 0.0) r.avg_paise = std::llround(avg * 100.0);
        const std::string msg = hit->str("message");
        if (!msg.empty()) r.message = msg;
        if (r.status != before.status || r.filled != before.filled || r.fyers_id != before.fyers_id
            || r.message != before.message || r.avg_paise != before.avg_paise) {
            r.updated_ns = now_ns;
            ++changed;
        }
    }
    return changed;
}

/// Today's P&L from a FYERS positions reply: overall.pl_total, else the sum
/// over netPositions. nullopt when the reply says neither.
[[nodiscard]] inline std::optional<std::int64_t> router_day_pnl_paise(const RouterJson& reply) {
    if (reply.str("s") != "ok") return std::nullopt;
    if (const RouterJson* o = reply.get("overall"); o != nullptr && o->has_num("pl_total"))
        return std::llround(o->num("pl_total") * 100.0);
    const RouterJson* net = reply.get("netPositions");
    if (net == nullptr || net->kind != RouterJson::Kind::Array) return std::nullopt;
    double sum = 0.0;
    for (const RouterJson& p : net->items)
        sum += p.has_num("pl") ? p.num("pl") : p.num("realized_profit") + p.num("unrealized_profit");
    return std::llround(sum * 100.0);
}

/// The last price of `symbol` from a FYERS /data/quotes reply.
[[nodiscard]] inline std::optional<std::int64_t> router_ltp_paise(const RouterJson& reply, std::string_view symbol) {
    const RouterJson* d = reply.get("d");
    if (reply.str("s") != "ok" || d == nullptr || d->kind != RouterJson::Kind::Array) return std::nullopt;
    for (const RouterJson& q : d->items) {
        const RouterJson* v = q.get("v");
        if (q.str("n") != symbol || v == nullptr) continue;
        const double lp = v->num("lp", 0.0);
        if (lp > 0.0) return std::llround(lp * 100.0);
    }
    return std::nullopt;
}

// ---- the transport, and the router ------------------------------------------------------

struct RouterCall {
    bool transport_ok = false;   ///< false: no HTTP exchange completed
    int status = 0;
    std::string body;
};

struct RouterTransport {
    /// GET https://api-t1.fyers.in<target> with the session.
    std::function<RouterCall(const std::string& target)> get;
    /// POST or DELETE https://api-t1.fyers.in<target> with a JSON body.
    std::function<RouterCall(const std::string& method, const std::string& target, const std::string& body)> send;
};

inline constexpr const char* kRouterOrderPath = "/api/v3/orders/sync";
inline constexpr const char* kRouterOrderBookPath = "/api/v3/orders";
inline constexpr const char* kRouterPositionsPath = "/api/v3/positions";
inline constexpr const char* kRouterQuotesPath = "/data/quotes?symbols=";

[[nodiscard]] inline std::string router_url_escape(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
            || c == '~') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

struct RouterPaths {
    std::string intents, drainer_state, arm, kill, cancels, orders, journal;
    [[nodiscard]] static RouterPaths under(const std::string& root) {
        const std::string dir = root + "/" + kRouterDir;
        return RouterPaths{root + "/" + kIntentFile, dir + "/intent_cursor.txt", root + "/" + kRouterArmFile,
                           root + "/" + kRouterKillFile, dir + "/cancels.jsonl", dir + "/orders.json",
                           dir + "/journal.jsonl"};
    }
};

struct RouterStatus {
    bool armed = false, killed = true, dry_run = false, session = false;
    std::string why;                              ///< why requests are refused now; empty when they are not
    RouterArm arm;
    std::optional<std::int64_t> day_pnl_paise;
    std::int64_t orders_today = 0, open = 0;
    std::int64_t beat_ns = 0;
};

class OrderRouter {
public:
    OrderRouter(RouterPaths paths, std::vector<RouterInstrument> universe, RouterTransport transport,
                std::uint64_t session_generation, bool dry_run, std::int64_t now_ns)
        : p_(std::move(paths)), u_(std::move(universe)), t_(std::move(transport)),
          generation_(session_generation), dry_run_(dry_run) {
        status_.dry_run = dry_run;
        status_.session = generation_ != 0;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(p_.orders).parent_path(), ec);
        auto d = IntentDrainer::create();
        if (!d) { error_ = "the intent drainer refused its policy"; return; }
        drainer_.emplace(std::move(*d));
        if (auto c = load_drainer_state(p_.drainer_state, *drainer_)) {
            cursor_ = *c;
        } else if (c.error() != QueueError::NoState) {
            error_ = "the saved intent cursor " + p_.drainer_state
                   + " cannot be read; it is not reset by itself, because that could re-send a request. "
                     "Move it aside if no order router has run in the last minute.";
            return;
        }
        // Cancels written before this start are not this run's to act on.
        cancel_offset_ = static_cast<std::uint64_t>(std::filesystem::file_size(p_.cancels, ec));
        if (ec) cancel_offset_ = 0;
        load_rows(now_ns);
        journal(now_ns, "start", dry_run_ ? "\"dry_run\":true" : "\"dry_run\":false");
    }

    [[nodiscard]] const std::string& startup_error() const noexcept { return error_; }
    [[nodiscard]] const std::vector<RouterOrder>& rows() const noexcept { return rows_; }
    [[nodiscard]] const RouterStatus& status() const noexcept { return status_; }

    void set_universe(std::vector<RouterInstrument> u) { u_ = std::move(u); }
    void set_session(std::uint64_t generation) {
        if (generation != generation_) { auth_ok_ns_ = 0; account_ok_ns_ = 0; day_pnl_.reset(); }
        generation_ = generation;
        status_.session = generation_ != 0;
    }

    /// One pass: switches, kill, requests, cancels, the order book, the P&L,
    /// then the file the Terminal reads.
    void tick(std::int64_t now_ns) {
        if (!error_.empty()) return;
        read_switches(now_ns);
        if (killed_) cancel_open(now_ns, "the kill switch is on");
        drain_requests(now_ns);
        drain_cancels(now_ns);
        if (generation_ != 0 && now_ns >= next_book_ns_ && any_open()) poll_book(now_ns);
        if (generation_ != 0 && arm_.verdict == RouterArmVerdict::Armed && now_ns - pnl_ns_ >= 15'000'000'000LL)
            refresh_pnl(now_ns);
        publish(now_ns);
    }

private:
    // ---- switches -------------------------------------------------------------
    void read_switches(std::int64_t now_ns) {
        const std::string text = router_detail::file_text(p_.arm);
        std::error_code ec;
        const bool arm_there = std::filesystem::exists(p_.arm, ec);
        if (ec) {
            arm_ = RouterArmRead{RouterArmVerdict::Unreadable, {}, "the arm file cannot be checked: live trading stays off"};
        } else if (!arm_there) {
            arm_ = RouterArmRead{RouterArmVerdict::Absent, {}, "live trading is off (the Terminal's LIVE switch)"};
        } else {
            arm_ = read_router_arm(text, router_detail::floor_div(now_ns, 1'000'000'000LL));
        }
        const std::uint64_t rev = detail::fnv1a(text.data(), text.size());
        arm_revision_ = rev == 0 ? 1 : rev;
        std::error_code kec;
        const bool kill_there = std::filesystem::exists(p_.kill, kec);
        // A kill file that cannot be checked is a kill (desktop/kill_switch.hpp).
        const bool killed = kec || kill_there;
        if (killed && !killed_) journal(now_ns, "kill", "\"on\":true");
        if (!killed && killed_ && started_) journal(now_ns, "kill", "\"on\":false");
        killed_ = killed;
        started_ = true;
    }

    [[nodiscard]] std::string refusal_now() const {
        if (killed_) return "the kill switch is on (Operations > Kill switch)";
        if (arm_.verdict != RouterArmVerdict::Armed) return arm_.why;
        if (generation_ == 0) return "no FYERS session: log in on the Brokers page";
        return {};
    }

    // ---- requests ------------------------------------------------------------
    void drain_requests(std::int64_t now_ns) {
        std::error_code ec;
        if (!std::filesystem::exists(p_.intents, ec) || ec) return;
        for (int round = 0; round < 8; ++round) {
            auto batch = drainer_->drain(p_.intents, cursor_, now_ns);
            if (!batch) {
                if (batch.error() == QueueError::Truncated || batch.error() == QueueError::Replaced) {
                    // The file was replaced or cut: start it over. Ids already
                    // seen stay remembered, and anything older than the TTL expires.
                    journal(now_ns, "queue", "\"restart\":\"the request file was replaced\"");
                    cursor_ = IntentCursor{};
                    continue;
                }
                return;
            }
            if (!save_drainer_state(p_.drainer_state, batch->next, *drainer_)) {
                drainer_->rollback(*batch);
                journal(now_ns, "queue", "\"error\":\"the intent cursor could not be saved; nothing was acted on\"");
                return;
            }
            cursor_ = batch->next;
            if (batch->refused.total() > 0)
                journal(now_ns, "queue", "\"refused_lines\":" + std::to_string(batch->refused.total())
                                             + ",\"expired\":" + std::to_string(batch->refused.expired));
            for (const OrderIntent& in : batch->accepted) handle(in, now_ns);
            if (!batch->more) return;
        }
    }

    void handle(const OrderIntent& in, std::int64_t now_ns) {
        RouterOrder r;
        r.intent_id = in.id;
        r.tag = router_order_tag(in.id);
        r.symbol = in.symbol;
        r.side = in.side == IntentSide::Buy ? "BUY" : "SELL";
        r.type = in.order_type == IntentType::Limit ? "LIMIT" : "MARKET";
        r.product = in.product;
        r.by = in.by;
        r.lots = in.lots;
        r.limit_paise = in.limit_paise;
        r.at_ns = now_ns;
        r.updated_ns = now_ns;
        journal(now_ns, "request", "\"intent\":" + router_detail::quoted(in.id) + ",\"symbol\":" + router_detail::quoted(in.symbol)
                                       + ",\"side\":\"" + r.side + "\",\"lots\":" + std::to_string(in.lots));
        if (const std::string why = refusal_now(); !why.empty()) return refuse(std::move(r), why, now_ns);

        const RouterInstrument* inst = nullptr;
        for (const auto& x : u_)
            if (x.token == in.token) { inst = &x; break; }
        std::optional<std::int64_t> ltp;
        if (inst != nullptr) {
            r.fyers_symbol = inst->fyers;
            r.qty = in.lots * inst->lot;
            const RouterCall q = t_.get(std::string(kRouterQuotesPath) + router_url_escape(inst->fyers));
            note_auth(q, now_ns);
            if (q.transport_ok && q.status == 200)
                if (const auto j = parse_router_json(q.body)) ltp = router_ltp_paise(*j, inst->fyers);
        }
        if (now_ns - pnl_ns_ >= 5'000'000'000LL) refresh_pnl(now_ns);
        const RouterBook book{orders_today(now_ns), open_count(), day_pnl_};
        auto plan = plan_router_order(in, inst, arm_.arm, book, ltp, now_ns);
        if (!plan) return refuse(std::move(r), plan.error(), now_ns);
        r.qty = plan->qty;

        RouterEvidence ev{true, killed_, true, arm_revision_, generation_, auth_ok_ns_, account_ok_ns_, true, now_ns};
        const RouterGate g = router_gate(ev);
        const auto permit = authorize_broker_dispatch(g.routes, g.evidence, g.gate, Timestamp{now_ns});
        if (!permit)
            return refuse(std::move(r), auth_ok_ns_ == 0 ? "FYERS did not accept the session: log in again on the Brokers page"
                                                         : "the dispatch gate is shut (session, positions or arm not current)",
                          now_ns);
        // The mandatory recheck, as late as possible: the switches are read
        // again, and a changed arm, a kill or a new login voids the permit.
        read_switches(now_ns);
        ev.armed = arm_.verdict == RouterArmVerdict::Armed;
        ev.killed = killed_;
        ev.arm_revision = arm_revision_;
        ev.session_generation = generation_;
        const RouterGate g2 = router_gate(ev);
        if (!permit_matches(*permit, g2.routes, g2.evidence, g2.gate, Timestamp{now_ns}))
            return refuse(std::move(r), "the arm, kill switch or session changed while the order was checked: not sent", now_ns);

        if (dry_run_) {
            r.status = RouterOrderStatus::DryRun;
            r.message = "dry run: would send " + plan->body;
            journal(now_ns, "dry_run", "\"intent\":" + router_detail::quoted(in.id) + ",\"body\":" + plan->body);
            rows_.push_back(std::move(r));
            return;
        }
        journal(now_ns, "send", "\"intent\":" + router_detail::quoted(in.id) + ",\"body\":" + plan->body);
        const RouterCall c = t_.send("POST", kRouterOrderPath, plan->body);
        note_auth(c, now_ns);
        std::string fy_id, fy_msg, fy_s;
        if (c.transport_ok) {
            if (const auto j = parse_router_json(c.body)) {
                fy_id = j->str("id");
                fy_msg = j->str("message");
                fy_s = j->str("s");
            }
        }
        const bool acked = fy_s == "ok" && !fy_id.empty();
        const auto disposition = c.transport_ok ? fyers_dispatch_disposition(c.status, acked)
                                                : FyersDispatchDisposition::ReconcileRequired;
        journal(now_ns, "reply", "\"intent\":" + router_detail::quoted(in.id) + ",\"http\":" + std::to_string(c.status)
                                     + ",\"id\":" + router_detail::quoted(fy_id) + ",\"message\":" + router_detail::quoted(fy_msg));
        switch (disposition) {
        case FyersDispatchDisposition::Accepted:
            r.status = RouterOrderStatus::Pending;
            r.fyers_id = fy_id;
            r.message = fy_msg.empty() ? "accepted by FYERS" : fy_msg;
            break;
        case FyersDispatchDisposition::Rejected:
            r.status = RouterOrderStatus::Rejected;
            r.message = fy_msg.empty() ? "FYERS refused it (HTTP " + std::to_string(c.status) + ")" : fy_msg;
            break;
        case FyersDispatchDisposition::ReconcileRequired:
            r.status = RouterOrderStatus::Uncertain;
            r.message = (c.transport_ok ? (fy_msg.empty() ? "no order id in the reply" : fy_msg) : std::string("no reply from FYERS"))
                      + ": looking for it in the order book (never re-sent)";
            break;
        }
        r.updated_ns = now_ns;
        rows_.push_back(std::move(r));
        next_book_ns_ = now_ns + 500'000'000LL;   // read the book on the next pass
        dirty_ = true;
    }

    void refuse(RouterOrder r, const std::string& why, std::int64_t now_ns) {
        r.status = RouterOrderStatus::Refused;
        r.message = why;
        r.updated_ns = now_ns;
        journal(now_ns, "refused", "\"intent\":" + router_detail::quoted(r.intent_id) + ",\"why\":" + router_detail::quoted(why));
        rows_.push_back(std::move(r));
        dirty_ = true;
    }

    // ---- cancels ---------------------------------------------------------------
    void drain_cancels(std::int64_t now_ns) {
        std::ifstream f(p_.cancels, std::ios::binary);
        if (!f) return;
        f.seekg(0, std::ios::end);
        const auto size = static_cast<std::uint64_t>(std::max<std::streamoff>(0, f.tellg()));
        if (size < cancel_offset_) cancel_offset_ = 0;   // replaced: read it from the start
        if (size == cancel_offset_) return;
        f.seekg(static_cast<std::streamoff>(cancel_offset_), std::ios::beg);
        std::string chunk(static_cast<std::size_t>(std::min<std::uint64_t>(size - cancel_offset_, 1u << 20)), '\0');
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        chunk.resize(static_cast<std::size_t>(f.gcount()));
        const std::size_t last_nl = chunk.rfind('\n');
        if (last_nl == std::string::npos) return;   // a line still being written
        cancel_offset_ += last_nl + 1;
        std::istringstream lines(chunk.substr(0, last_nl + 1));
        std::string line;
        while (std::getline(lines, line)) {
            const auto j = parse_router_json(line);
            if (!j) continue;
            if (j->truth("all")) { cancel_open(now_ns, "cancel all, from the Terminal"); continue; }
            const std::string id = j->str("id");
            for (RouterOrder& r : rows_)
                if (!id.empty() && r.fyers_id == id && !router_final(r.status)) send_cancel(r, now_ns, "cancel, from the Terminal");
        }
    }

    void cancel_open(std::int64_t now_ns, const char* why) {
        for (RouterOrder& r : rows_)
            if (!r.cancel_sent && !r.fyers_id.empty() && !router_final(r.status)) send_cancel(r, now_ns, why);
    }

    void send_cancel(RouterOrder& r, std::int64_t now_ns, const char* why) {
        if (dry_run_ || generation_ == 0) return;
        const std::string body = "{\"id\":" + router_detail::quoted(r.fyers_id) + "}";
        const RouterCall c = t_.send("DELETE", kRouterOrderPath, body);
        note_auth(c, now_ns);
        r.cancel_sent = true;
        std::string msg;
        if (c.transport_ok)
            if (const auto j = parse_router_json(c.body)) msg = j->str("message");
        r.message = std::string(why) + ": " + (c.transport_ok ? (msg.empty() ? "sent" : msg) : "no reply; the order book will tell");
        r.updated_ns = now_ns;
        journal(now_ns, "cancel", "\"id\":" + router_detail::quoted(r.fyers_id) + ",\"http\":" + std::to_string(c.status)
                                      + ",\"message\":" + router_detail::quoted(msg));
        next_book_ns_ = now_ns + 500'000'000LL;
        dirty_ = true;
    }

    // ---- the order book and the P&L -------------------------------------------------
    [[nodiscard]] bool any_open() const {
        return std::any_of(rows_.begin(), rows_.end(), [](const RouterOrder& r) { return !router_final(r.status); });
    }
    [[nodiscard]] std::int64_t open_count() const {
        return std::count_if(rows_.begin(), rows_.end(), [](const RouterOrder& r) { return !router_final(r.status); });
    }
    [[nodiscard]] std::int64_t orders_today(std::int64_t now_ns) const {
        const std::int64_t today = router_ist(now_ns).day;
        return std::count_if(rows_.begin(), rows_.end(), [today](const RouterOrder& r) {
            return router_ist(r.at_ns).day == today && r.status != RouterOrderStatus::Refused
                && r.status != RouterOrderStatus::DryRun && r.status != RouterOrderStatus::NotPlaced;
        });
    }

    void poll_book(std::int64_t now_ns) {
        next_book_ns_ = now_ns + 2'000'000'000LL;
        const RouterCall c = t_.get(kRouterOrderBookPath);
        note_auth(c, now_ns);
        if (!c.transport_ok || c.status != 200) return;
        const auto j = parse_router_json(c.body);
        if (!j) return;
        const std::size_t n = apply_router_book(rows_, *j, now_ns);
        if (n > 0) {
            dirty_ = true;
            for (const RouterOrder& r : rows_)
                if (r.updated_ns == now_ns)
                    journal(now_ns, "status", "\"intent\":" + router_detail::quoted(r.intent_id) + ",\"id\":"
                                                  + router_detail::quoted(r.fyers_id) + ",\"status\":\""
                                                  + router_status_text(r.status) + "\",\"filled\":" + std::to_string(r.filled));
        }
        // A send that turned up open after the kill is cancelled now.
        if (killed_) cancel_open(now_ns, "the kill switch is on");
    }

    void refresh_pnl(std::int64_t now_ns) {
        pnl_ns_ = now_ns;
        const RouterCall c = t_.get(kRouterPositionsPath);
        note_auth(c, now_ns);
        std::optional<std::int64_t> pnl;
        if (c.transport_ok && c.status == 200)
            if (const auto j = parse_router_json(c.body)) pnl = router_day_pnl_paise(*j);
        day_pnl_ = pnl;
        if (pnl) account_ok_ns_ = now_ns;
    }

    void note_auth(const RouterCall& c, std::int64_t now_ns) {
        if (!c.transport_ok) return;
        if (c.status == 200) auth_ok_ns_ = now_ns;
        else if (c.status == 401 || c.status == 403) auth_ok_ns_ = 0;
    }

    // ---- files ----------------------------------------------------------------------
    void journal(std::int64_t now_ns, const char* event, const std::string& fields) {
        std::ofstream f(p_.journal, std::ios::binary | std::ios::app);
        f << "{\"ns\":" << now_ns << ",\"event\":\"" << event << "\"," << fields << "}\n";
    }

    void load_rows(std::int64_t now_ns) {
        const auto j = parse_router_json(router_detail::file_text(p_.orders));
        if (!j) return;
        const RouterJson* orders = j->get("orders");
        if (orders == nullptr || orders->kind != RouterJson::Kind::Array) return;
        const std::int64_t today = router_ist(now_ns).day;
        for (const RouterJson& o : orders->items) {
            RouterOrder r;
            r.at_ns = static_cast<std::int64_t>(o.num("at_ns"));
            // RULE 11: only today's rows are carried: yesterday's orders are FYERS's history, not this run's.
            if (router_ist(r.at_ns).day != today) continue;
            const auto st = router_status_from_text(o.str("status"));
            if (!st) continue;
            r.status = *st;
            r.intent_id = o.str("intent");
            r.tag = o.str("tag");
            r.fyers_id = o.str("id");
            r.symbol = o.str("symbol");
            r.fyers_symbol = o.str("fyers");
            r.side = o.str("side");
            r.type = o.str("type");
            r.product = o.str("product");
            r.by = o.str("by");
            r.lots = static_cast<std::int64_t>(o.num("lots"));
            r.qty = static_cast<std::int64_t>(o.num("qty"));
            r.limit_paise = static_cast<std::int64_t>(o.num("limit_paise"));
            r.filled = static_cast<std::int64_t>(o.num("filled"));
            r.avg_paise = static_cast<std::int64_t>(o.num("avg_paise"));
            r.message = o.str("message");
            r.updated_ns = static_cast<std::int64_t>(o.num("updated_ns"));
            r.cancel_sent = o.truth("cancel_sent");
            rows_.push_back(std::move(r));
        }
        next_book_ns_ = now_ns;
    }

    void publish(std::int64_t now_ns) {
        status_.armed = arm_.verdict == RouterArmVerdict::Armed;
        status_.killed = killed_;
        status_.why = refusal_now();
        status_.arm = arm_.arm;
        status_.day_pnl_paise = day_pnl_;
        status_.orders_today = orders_today(now_ns);
        status_.open = open_count();
        // The heartbeat is the file's age: written at least every two seconds.
        if (!dirty_ && now_ns - status_.beat_ns < 2'000'000'000LL) return;
        status_.beat_ns = now_ns;
        dirty_ = false;
        const auto q = router_detail::quoted;
        std::string s = "{\"router\":{\"beat_ns\":" + std::to_string(now_ns) + ",\"armed\":" + (status_.armed ? "true" : "false")
                      + ",\"killed\":" + (killed_ ? "true" : "false") + ",\"dry_run\":" + (dry_run_ ? "true" : "false")
                      + ",\"session\":" + (generation_ != 0 ? "true" : "false") + ",\"why\":" + q(status_.why)
                      + ",\"day_pnl_paise\":" + (day_pnl_ ? std::to_string(*day_pnl_) : std::string("null"))
                      + ",\"orders_today\":" + std::to_string(status_.orders_today) + ",\"open\":" + std::to_string(status_.open)
                      + ",\"max_orders_per_day\":" + std::to_string(arm_.arm.max_orders_per_day)
                      + ",\"max_daily_loss_paise\":" + std::to_string(arm_.arm.max_daily_loss_paise) + "},\n\"orders\":[";
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const RouterOrder& r = rows_[i];
            s += (i ? ",\n " : "\n ");
            s += "{\"at_ns\":" + std::to_string(r.at_ns) + ",\"updated_ns\":" + std::to_string(r.updated_ns) + ",\"intent\":"
               + q(r.intent_id) + ",\"tag\":" + q(r.tag) + ",\"id\":" + q(r.fyers_id) + ",\"symbol\":" + q(r.symbol)
               + ",\"fyers\":" + q(r.fyers_symbol) + ",\"side\":" + q(r.side) + ",\"type\":" + q(r.type) + ",\"product\":"
               + q(r.product) + ",\"by\":" + q(r.by) + ",\"lots\":" + std::to_string(r.lots) + ",\"qty\":" + std::to_string(r.qty)
               + ",\"limit_paise\":" + std::to_string(r.limit_paise) + ",\"filled\":" + std::to_string(r.filled)
               + ",\"avg_paise\":" + std::to_string(r.avg_paise) + ",\"status\":\"" + router_status_text(r.status)
               + "\",\"cancel_sent\":" + (r.cancel_sent ? "true" : "false") + ",\"message\":" + q(r.message) + "}";
        }
        s += "\n]}\n";
        const std::string tmp = p_.orders + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << s;
            if (!f) return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, p_.orders, ec);
        if (ec) dirty_ = true;   // the Terminal held the file open; written on the next pass
    }

    RouterPaths p_;
    std::vector<RouterInstrument> u_;
    RouterTransport t_;
    std::uint64_t generation_ = 0;
    bool dry_run_ = false;
    std::string error_;
    std::optional<IntentDrainer> drainer_;
    IntentCursor cursor_{};
    std::uint64_t cancel_offset_ = 0;
    RouterArmRead arm_;
    std::uint64_t arm_revision_ = 1;
    bool killed_ = true;
    bool started_ = false;
    std::vector<RouterOrder> rows_;
    std::optional<std::int64_t> day_pnl_;
    std::int64_t pnl_ns_ = std::numeric_limits<std::int64_t>::min() / 2;
    std::int64_t auth_ok_ns_ = 0, account_ok_ns_ = 0;
    std::int64_t next_book_ns_ = 0;
    bool dirty_ = true;
    RouterStatus status_;
};

} // namespace altair::oms
