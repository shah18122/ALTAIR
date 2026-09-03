// risk/charges_toml.cpp -- the TOML half of P3-09b.
//
// The only file in risk/ that links a third-party parser, kept in a .cpp for
// the same reason core/config/toml_source.cpp is: `risk/cost.hpp` is included
// by strategies and by ALTAIR_HOT code, and must not drag tomlplusplus in.

#include <risk/charges_toml.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace altair {
namespace {

/// Days from 1970-01-01 to y-m-d, proleptic Gregorian (Howard Hinnant).
///
/// DUPLICATED from `instruments/kite_dump.hpp` on purpose. CLAUDE.md: a card's
/// manifest never spans two directories, and risk/ taking a dependency on
/// instruments/ to borrow eight lines of arithmetic would be a worse trade
/// than writing them twice. It is a closed-form standard algorithm with no
/// state and no configuration, so the two copies cannot drift in behaviour --
/// which is the usual reason duplication is dangerous and is absent here.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m,
                                       unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;

/// "YYYY-MM-DD" -> nanoseconds since the epoch at UTC midnight of that day.
///
/// Returns false on anything else. A date that does not parse is refused, not
/// guessed at: a schedule with a wrong effective range charges a backtest the
/// wrong decade's rates and nothing downstream can tell.
bool parse_date(std::string_view s, std::int64_t& out_ns) noexcept {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return false;
    }
    std::int64_t v[3] = {0, 0, 0};
    const std::size_t at[3] = {0, 5, 8};
    const std::size_t len[3] = {4, 2, 2};
    for (int f = 0; f < 3; ++f) {
        for (std::size_t i = 0; i < len[f]; ++i) {
            const char c = s[at[f] + i];
            if (c < '0' || c > '9') {
                return false;
            }
            v[f] = v[f] * 10 + (c - '0');
        }
    }
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) {
        return false;
    }
    out_ns = days_from_civil(v[0], static_cast<unsigned>(v[1]),
                             static_cast<unsigned>(v[2])) * kNsPerDay;
    return true;
}

bool parse_side(std::string_view s, ChargeSide& out) noexcept {
    if (s == "buy")  { out = ChargeSide::Buy;  return true; }
    if (s == "sell") { out = ChargeSide::Sell; return true; }
    if (s == "both") { out = ChargeSide::Both; return true; }
    if (s == "none") { out = ChargeSide::None; return true; }
    return false;
}

bool parse_basis(std::string_view s, TurnoverBasis& out) noexcept {
    if (s == "notional") { out = TurnoverBasis::Notional; return true; }
    if (s == "premium")  { out = TurnoverBasis::Premium;  return true; }
    return false;
}

/// Read a rate. ABSENT is fine and leaves the default; PRESENT AND BAD is an
/// error. That asymmetry is the whole safety argument of this file: a rate
/// that fails to parse and silently becomes zero makes every strategy look
/// more profitable, and nobody investigates that direction.
bool rate_field(const toml::table& t, const char* key, RateNano& out,
                bool& bad) noexcept {
    const auto* n = t.get(key);
    if (n == nullptr) {
        return false;               // absent: caller keeps the default
    }
    const auto v = n->value<double>();
    if (!v || *v < 0.0) {
        bad = true;
        return false;
    }
    out = rate_from(static_cast<long double>(*v));
    return true;
}

/// One `[schedule.<segment>]` table.
bool read_segment(const toml::table& sch, const char* name,
                  SegmentCharges& out, const char* stt_key,
                  const char* stt_side_key, const char* exch_nse_key,
                  ChargesError& err) noexcept {
    const auto* node = sch.get(name);
    if (node == nullptr) {
        // ABSENT, not empty. `present` stays false and compute_cost returns
        // UnknownSegment rather than pricing the trade at zero.
        return true;
    }
    const auto* t = node->as_table();
    if (t == nullptr) {
        err = ChargesError::ParseFailed;
        return false;
    }

    bool bad = false;
    rate_field(*t, stt_key, out.stt, bad);
    rate_field(*t, exch_nse_key, out.exch_txn_nse, bad);
    rate_field(*t, "exch_txn_bse", out.exch_txn_bse, bad);
    rate_field(*t, "stamp_duty", out.stamp, bad);
    rate_field(*t, "ipft_nse", out.ipft, bad);
    if (bad) {
        err = ChargesError::BadRate;
        return false;
    }

    if (const auto* s = t->get(stt_side_key)) {
        const auto v = s->value<std::string_view>();
        if (!v || !parse_side(*v, out.stt_side)) {
            err = ChargesError::BadEnum;
            return false;
        }
    }
    if (const auto* s = t->get("stamp_side")) {
        const auto v = s->value<std::string_view>();
        if (!v || !parse_side(*v, out.stamp_side)) {
            err = ChargesError::BadEnum;
            return false;
        }
    }
    if (const auto* s = t->get("turnover_basis")) {
        const auto v = s->value<std::string_view>();
        if (!v || !parse_basis(*v, out.basis)) {
            err = ChargesError::BadEnum;
            return false;
        }
    }

    // Flat rupee amount -> paise. Written as rupees in the file because that
    // is how the broker publishes it; converted here so nothing downstream
    // handles a rupee figure.
    if (const auto* d = t->get("dp_charge_per_scrip_sell")) {
        const auto v = d->value<double>();
        if (!v || *v < 0.0) {
            err = ChargesError::BadRate;
            return false;
        }
        out.dp_per_scrip_sell =
            Notional{static_cast<std::int64_t>(*v * 100.0 + 0.5)};
    }

    out.present = true;
    return true;
}

std::expected<ChargesLoadReport, ChargesError>
load_document(const toml::table& root,
              std::vector<ChargeSchedule>& out) noexcept {
    out.clear();
    ChargesLoadReport rep{};

    // D7 -- verification propagates from the file and from nowhere else.
    if (const auto* lv = root.get("last_verified")) {
        const auto v = lv->value<std::string_view>();
        rep.verified = v && !v->empty() && *v != "UNVERIFIED";
    }

    RateNano gst = rate_from(0.18L);
    RateNano sebi = rate_from(0.000001L);
    if (const auto* c = root.get("common")) {
        if (const auto* ct = c->as_table()) {
            bool bad = false;
            rate_field(*ct, "gst_rate", gst, bad);
            if (const auto* s = ct->get("sebi_turnover_per_cr")) {
                const auto v = s->value<double>();
                if (!v || *v < 0.0) {
                    bad = true;
                } else {
                    // Rupees per crore -> a fraction of turnover. Rs 10 per
                    // Rs 1,00,00,000 is 1e-6. Written in the file the way the
                    // circular writes it; converted once, here.
                    sebi = rate_from(static_cast<long double>(*v) / 1e7L);
                }
            }
            if (bad) {
                return std::unexpected(ChargesError::BadRate);
            }
        }
    }

    const auto* arr_node = root.get("schedule");
    const toml::array* arr = arr_node != nullptr ? arr_node->as_array() : nullptr;
    if (arr == nullptr || arr->empty()) {
        return std::unexpected(ChargesError::NoSchedules);
    }

    for (const auto& elem : *arr) {
        const auto* st = elem.as_table();
        if (st == nullptr) {
            return std::unexpected(ChargesError::ParseFailed);
        }
        ChargeSchedule s{};
        s.gst = gst;
        s.sebi = sebi;
        s.verified = rep.verified;

        const auto* vf = st->get("valid_from");
        const auto* vt = st->get("valid_to");
        if (vf == nullptr || vt == nullptr) {
            return std::unexpected(ChargesError::BadDateRange);
        }
        const auto vfs = vf->value<std::string_view>();
        const auto vts = vt->value<std::string_view>();
        std::int64_t from_ns = 0;
        std::int64_t to_ns = 0;
        if (!vfs || !vts || !parse_date(*vfs, from_ns)
            || !parse_date(*vts, to_ns)) {
            return std::unexpected(ChargesError::BadDateRange);
        }
        // THE BOUNDARY THIS FILE MOST EASILY GETS WRONG.
        //
        // `schedule_for` matches an INCLUSIVE range, and `valid_to` in the
        // file is a DAY -- "2026-03-31" means all of that day. Parsed to
        // midnight, a trade at 09:15 on 31 March falls after schedule 1's
        // valid_to and before schedule 2's valid_from, and gets NoSchedule:
        // the last trading day of the old STT regime would be unpriceable.
        // So valid_to becomes that day's LAST nanosecond.
        to_ns += kNsPerDay - 1;
        if (to_ns < from_ns) {
            return std::unexpected(ChargesError::BadDateRange);
        }
        s.valid_from = Timestamp{from_ns};
        s.valid_to = Timestamp{to_ns};

        ChargesError err = ChargesError::ParseFailed;
        struct Line { const char* name; SegmentCharges* out; const char* stt;
                      const char* side; const char* exch; };
        const Line lines[] = {
            {"equity_delivery",   &s.equity_delivery,   "stt_rate", "stt_side", "exch_txn_nse"},
            {"equity_intraday",   &s.equity_intraday,   "stt_rate", "stt_side", "exch_txn_nse"},
            {"equity_futures",    &s.equity_futures,    "stt_rate", "stt_side", "exch_txn_nse"},
            {"equity_options",    &s.equity_options,    "stt_rate", "stt_side", "exch_txn_nse"},
            {"currency_futures",  &s.currency_futures,  "stt_rate", "stt_side", "exch_txn_nse"},
            {"currency_options",  &s.currency_options,  "stt_rate", "stt_side", "exch_txn_nse"},
            // Commodities pay CTT, not STT, and it is levied on MCX. Different
            // key names in the file because they are different taxes; the same
            // field here because the arithmetic is identical.
            {"commodity_futures", &s.commodity_futures, "ctt_rate", "ctt_side", "exch_txn_mcx"},
        };
        for (const Line& l : lines) {
            if (!read_segment(*st, l.name, *l.out, l.stt, l.side, l.exch,
                              err)) {
                return std::unexpected(err);
            }
            if (l.out->present) {
                ++rep.segment_tables;
            }
        }
        out.push_back(s);
        ++rep.schedules;
    }

    // Ascending, then checked for overlap. `schedule_for` returns the FIRST
    // match, so two schedules covering one instant silently decide which rates
    // a backtest gets -- and the decision would be file order, which nobody
    // thinks of as significant.
    for (std::size_t i = 1; i < out.size(); ++i) {
        for (std::size_t j = i; j > 0
             && out[j].valid_from.ns_since_epoch() < out[j - 1].valid_from.ns_since_epoch(); --j) {
            const ChargeSchedule tmp = out[j];
            out[j] = out[j - 1];
            out[j - 1] = tmp;
        }
    }
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (!(out[i - 1].valid_to.ns_since_epoch() < out[i].valid_from.ns_since_epoch())) {
            return std::unexpected(ChargesError::OverlappingSchedules);
        }
    }

    if (!out.empty()) {
        // How far the file's coverage extends. A trade after this is refused
        // by schedule_for, which is correct and worth surfacing BEFORE the
        // refusal rather than as a runtime surprise.
        const std::int64_t last = out.back().valid_to.ns_since_epoch();
        rep.uncovered_after_days = last / kNsPerDay;
    }
    return rep;
}

} // namespace

std::expected<ChargesLoadReport, ChargesError>
load_charges(const char* text, std::size_t len,
             std::vector<ChargeSchedule>& out) noexcept {
    if (len > kMaxChargesBytes) {
        return std::unexpected(ChargesError::FileTooLarge);
    }
    try {
        const toml::table root = toml::parse(std::string_view{text, len});
        return load_document(root, out);
    } catch (...) {
        // toml++ throws; this boundary is where that stops. risk/ is included
        // by hot-path code and CLAUDE.md rule 4 forbids exceptions there --
        // this function is cold, but the contract is the same either way.
        return std::unexpected(ChargesError::ParseFailed);
    }
}

std::expected<ChargesLoadReport, ChargesError>
load_charges_file(const char* path,
                  std::vector<ChargeSchedule>& out) noexcept {
    #ifdef _MSC_VER
    std::FILE* f = nullptr;
    if (::fopen_s(&f, path, "rb") != 0) { f = nullptr; }
#else
    std::FILE* f = std::fopen(path, "rb");
#endif
    if (f == nullptr) {
        return std::unexpected(ChargesError::FileNotFound);
    }
    std::string buf;
    try {
        char chunk[8192];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (buf.size() + n > kMaxChargesBytes) {
                std::fclose(f);
                return std::unexpected(ChargesError::FileTooLarge);
            }
            buf.append(chunk, n);
        }
    } catch (...) {
        std::fclose(f);
        return std::unexpected(ChargesError::ParseFailed);
    }
    std::fclose(f);
    return load_charges(buf.data(), buf.size(), out);
}

const char* charges_error_text(ChargesError e) noexcept {
    switch (e) {
        case ChargesError::FileNotFound:  return "charges file not found";
        case ChargesError::FileTooLarge:  return "charges file too large";
        case ChargesError::ParseFailed:   return "not valid TOML";
        case ChargesError::BadDateRange:  return "missing or unparseable valid_from/valid_to";
        case ChargesError::BadRate:       return "a rate is present but not a non-negative number";
        case ChargesError::BadEnum:       return "a side or turnover_basis is not a recognised value";
        case ChargesError::NoSchedules:   return "no [[schedule]] blocks";
        case ChargesError::OverlappingSchedules: return "two schedules cover the same instant";
    }
    return "unknown";
}

} // namespace altair
