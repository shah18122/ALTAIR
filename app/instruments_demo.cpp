// app/instruments_demo.cpp — what Phase 1 actually does, run end to end.
//
// Four stages, each one card:
//
//   P1-08a  the download-failure policy      may we use yesterday's file?
//   P1-04   the Kite dump parser             text -> ContractSpec, no floats
//   P1-06   the three-way reconciler         who is right when they disagree?
//   P1-01   the point-in-time spec store     resolve a token, honour a block
//
// This is a demonstration, not a test — the tests already passed. It exists so
// the pipeline can be watched rather than inferred from a green ctest line.

#include "instruments_demo.hpp"

#include <instruments/kite_dump.hpp>
#include <instruments/udiff_master.hpp>
#include <instruments/reconcile.hpp>
#include <instruments/snapshot.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace altair::demo {
namespace {

// ── plumbing ─────────────────────────────────────────────────────────────
// Reconciler is 5.25 MB and SpecStore 1.63 MB; neither goes on the stack.
Reconciler g_rec;
SpecStore g_store;

/// The demo universe. Production reads this from config [universe].
// October futures. Chosen because BOTH files spell a trading symbol the
// same way -- UDiFF FinInstrmNm and Kite tradingsymbol are both
// NIFTY26OCTFUT -- whereas ",NIFTY," matches only the exchange file:
// Kite QUOTES its name column, so the Kite side is ,"NIFTY",.
constexpr const char* kDemoUniverse = "26OCTFUT";

void rule(const char* title)
{
    // No ANSI. Windows consoles do not reliably interpret it, and a demo whose
    // headings render as "[1m" is worse than one with plain text.
    std::printf("\n%s\n", title);
    for (int i = 0; i < 74; ++i) {
        std::putchar('-');
    }
    std::putchar('\n');
}

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept
{
    y -= (m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned shifted = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * shifted + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr Timestamp ist_date(std::int64_t y, unsigned m, unsigned d) noexcept
{
    return Timestamp{days_from_civil(y, m, d) * 86'400'000'000'000LL
                     - kIstOffset.raw()};
}

const char* verdict_name(Verdict v)
{
    switch (v) {
        case Verdict::Agreed:         return "Agreed";
        case Verdict::SingleSource:   return "SingleSource";
        case Verdict::MissingPrimary: return "MissingPrimary";
        case Verdict::NoBroker:       return "NoBroker";
        case Verdict::ValueConflict:  return "ValueConflict";
    }
    return "?";
}

const char* snap_name(SnapshotVerdict v)
{
    switch (v) {
        case SnapshotVerdict::Fresh:         return "Fresh";
        case SnapshotVerdict::StaleUsable:   return "StaleUsable";
        case SnapshotVerdict::StaleAging:    return "StaleAging";
        case SnapshotVerdict::ExpiryCrossed: return "ExpiryCrossed";
        case SnapshotVerdict::TooOld:        return "TooOld";
        case SnapshotVerdict::NoSnapshot:    return "NoSnapshot";
    }
    return "?";
}

/// Rupees-and-paise from integer paise, without ever forming a double.
void print_paise(std::int64_t p, char* out, std::size_t cap)
{
    const char* sign = p < 0 ? "-" : "";
    const std::int64_t a = p < 0 ? -p : p;
    std::snprintf(out, cap, "%s%lld.%02lld", sign,
                  static_cast<long long>(a / 100),
                  static_cast<long long>(a % 100));
}

// ── a small built-in dump, so the demo runs with no files present ────────
// Real Kite schema, taken from gokiteconnect/market.go. Note tick_size 0.05:
// through a double that becomes 4 paise, not 5, and every rounded order price
// in the system is then wrong by a tick. That single value is why P1-04 exists.
constexpr const char* kBuiltinDump =
    "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
    "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
    "256265,1001,NIFTY,NIFTY,0,,0,0.05,1,EQ,NSE,NSE\n"
    "11223344,43528,NIFTY26SEP25000CE,NIFTY,0,2026-09-24,25000,0.05,75,CE,NFO-OPT,NFO\n"
    "11223355,43529,NIFTY26SEP25000PE,NIFTY,0,2026-09-24,25000,0.05,75,PE,NFO-OPT,NFO\n"
    "11223366,43530,NIFTY26SEP25100CE,NIFTY,0,2026-09-24,25100,0.05,75,CE,NFO-OPT,NFO\n"
    "11224400,43600,NIFTY26SEPFUT,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO\n"
    "12334455,44001,BANKNIFTY26SEPFUT,BANKNIFTY,0,2026-09-24,0,0.05,35,FUT,NFO-FUT,NFO\n";

/// Read a whole file. Returns nullptr on failure; caller frees with std::free.
char* slurp(const char* path, std::size_t& len)
{
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (::fopen_s(&f, path, "rb") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(path, "rb");
#endif
    if (f == nullptr) {
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return nullptr;
    }
    char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(n)));
    if (buf == nullptr) {
        std::fclose(f);
        return nullptr;
    }
    len = std::fread(buf, 1, static_cast<std::size_t>(n), f);
    std::fclose(f);
    return buf;
}

/// Keep the header plus rows whose text contains `needle`, in place.
///
/// A stand-in for the real universe filter. It exists because the capacity
/// debt is not theoretical: the live Kite dump is 106'150 rows and the NSE
/// bhavcopy 30'488, against a store of 8'192. Production filters to the
/// configured universe BEFORE adding; this filters by substring so the demo
/// can show the pipeline on a universe that fits.
std::size_t filter_rows(char* csv, std::size_t len, const char* needle)
{
    const std::size_t nlen = std::strlen(needle);
    std::size_t out = 0;
    std::size_t pos = 0;
    bool header = true;
    while (pos < len) {
        std::size_t e = pos;
        while (e < len && csv[e] != '\n') { ++e; }
        const std::size_t row = e - pos;
        bool keep = header;
        if (!header && row >= nlen) {
            for (std::size_t i = 0; i + nlen <= row && !keep; ++i) {
                if (std::memcmp(csv + pos + i, needle, nlen) == 0) { keep = true; }
            }
        }
        if (keep) {
            std::memmove(csv + out, csv + pos, row);
            out += row;
            csv[out++] = '\n';
        }
        header = false;
        pos = e < len ? e + 1 : len;
    }
    return out;
}

/// One synthetic exchange-master row, so the reconciler has a second opinion.
/// P1-02a will produce these for real; the shape is already fixed.
ContractSpec nse_row(const char* underlying, const char* symbol,
                     std::int64_t lot, std::int64_t tick, std::int64_t strike,
                     OptionType ot, Segment seg, Timestamp expiry,
                     Timestamp snap)
{
    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.lot_size = LotSize{lot};
    s.tick_size = Price{tick};
    s.strike = Price{strike};
    s.price_scale = 100;
    s.expiry = expiry;
    s.valid_from = snap;
    s.valid_to = Timestamp::max();
    s.source_hash = 0x5EED;
    s.snapshot_at = snap;
    s.exchange = Exchange::NSE;
    s.segment = seg;
    s.opt_type = ot;
    s.source = SpecSource::NseMaster;
    std::snprintf(s.symbol, sizeof(s.symbol), "%s", symbol);
    std::snprintf(s.underlying, sizeof(s.underlying), "%s", underlying);
    return s;
}

// ── stage 1 ──────────────────────────────────────────────────────────────
void stage_snapshot_policy(Timestamp today)
{
    rule("[1/4]  P1-08a  download-failure policy");
    std::printf(
        "The fetch failed at 08:15. May the session use what is on disk?\n"
        "The answer is NOT age alone -- it is age in TRADING days, and whether\n"
        "an F&O expiry has been crossed. Rollover is when lot sizes change.\n\n");

    // The expiry is on Friday 28th — the SAME date as the row-4 snapshot. That
    // is the case the closed lower bound exists for: a file stamped 28th was
    // fetched at 08:15 on the 28th, BEFORE that day's 15:30 expiry, so it is a
    // pre-rollover file even though it is only one trading day old.
    const Timestamp expiry = ist_date(2026, 8, 28);
    const Timestamp expiries[] = {expiry};
    const MarketCalendar cal{nullptr, 0, expiries, 1};
    const MarketCalendar bare{};
    const SnapshotPolicy pol{3};

    struct Row {
        const char* what;
        Timestamp taken;
        bool ok;
        const MarketCalendar* cal;
    };
    const Row rows[] = {
        {"today's fetch succeeded",        today,                 true,  &bare},
        {"failed, Friday's file (Mon now)", ist_date(2026, 8, 28), false, &bare},
        {"failed, 4 trading days old",     ist_date(2026, 8, 25), false, &bare},
        {"failed, 1 day old, ACROSS expiry", ist_date(2026, 8, 28), false, &cal},
        {"failed, nothing on disk",        Timestamp{},           false, &bare},
    };

    std::printf("  %-32s %-10s %-15s %s\n", "situation", "age(td)", "verdict", "session");
    for (const Row& r : rows) {
        SnapshotState st{r.taken, r.taken.ns_since_epoch() != 0, r.ok};
        const SnapshotVerdict v = judge_snapshot(st, today, *r.cal, pol);
        const std::int32_t age = st.present
            ? trading_days_between(r.taken, today, *r.cal) : 0;
        const char* action = verdict_halts(v)     ? "HALT"
                           : verdict_withholds(v) ? "source withheld"
                                                  : "proceeds";
        char agebuf[16];
        if (st.present) {
            std::snprintf(agebuf, sizeof(agebuf), "%d", age);
        } else {
            std::snprintf(agebuf, sizeof(agebuf), "-");
        }
        std::printf("  %-32s %-10s %-15s %s\n", r.what, agebuf, snap_name(v), action);
    }
    std::printf(
        "\n  Note row 4: ONE trading day old and still withheld, because an\n"
        "  expiry fell in between. Age alone would have called it usable and\n"
        "  sized every derivative from the previous cycle's lot table.\n");
}

// ── stage 2 ──────────────────────────────────────────────────────────────
bool stage_parse(const char* csv, std::size_t len, Timestamp snap, const char* origin)
{
    rule("[2/4]  P1-04  Kite dump -> ContractSpec");
    std::printf("  source: %s (%zu bytes)\n\n", origin, len);

    g_rec.clear();
    const auto rep = load_kite_dump(csv, len, g_rec, snap);
    if (!rep.has_value()) {
        std::printf("  parse failed: error %u\n",
                    static_cast<unsigned>(rep.error()));
        return false;
    }
    std::printf("  parsed        %zu contracts\n", rep->added);
    std::printf("  unparseable   %zu\n", rep->unparseable);
    std::printf("  refused       %zu\n", rep->rejected_by_sink);

    // The number the whole card exists for.
    const auto tick = parse_rupees_to_paise("0.05", 4);
    char buf[32];
    print_paise(tick->raw(), buf, sizeof(buf));
    std::printf("\n  tick_size \"0.05\" -> %lld paise (Rs %s)\n",
                static_cast<long long>(tick->raw()), buf);
    std::printf("  through a double that truncates to 4, and every rounded\n"
                "  order price in the engine is then wrong by a tick.\n");
    return true;
}

// ── stage 3 ──────────────────────────────────────────────────────────────
/// The real path: load an actual UDiFF bhavcopy as the primary source.
void stage_reconcile_real(const char* path, Timestamp snap)
{
    rule("[3/4]  P1-06  three-way reconciliation (REAL exchange master)");

    std::size_t n = 0;
    char* owned = slurp(path, n);
    if (owned == nullptr) {
        std::printf("  cannot read %s\n", path);
        return;
    }
    n = filter_rows(owned, n, kDemoUniverse);
    const auto r = load_udiff_master(owned, n, Exchange::NSE, g_rec, snap);
    if (!r) {
        std::printf("  parse failed: error %u\n",
                    static_cast<unsigned>(r.error()));
        std::free(owned);
        return;
    }
    std::printf("  exchange master: %s\n", path);
    std::printf("    added %zu   unparseable %zu   refused %zu\n",
                r->added, r->unparseable, r->rejected_by_sink);
    std::free(owned);

    const ReconcileReport rep = g_rec.reconcile();
    std::printf("\n  contracts %zu   agreed %zu   single %zu   BLOCKED %zu\n",
                rep.contracts, rep.agreed, rep.single_source, rep.blocked);
    std::printf("  primary source seen: %s\n",
                rep.primary_source_seen ? "yes" : "no");
    if (rep.blocked > 0) {
        std::printf("    missing primary %zu   no broker %zu   conflicts %zu\n",
                    rep.missing_primary, rep.no_broker, rep.conflicts);
    }

    std::printf("\n  %-26s %-16s %-9s %s\n",
                "contract", "verdict", "lot", "tick");
    std::size_t shown = 0;
    for (std::size_t i = 0; i < g_rec.size() && shown < 8; ++i) {
        const auto v = g_rec.verdict_at(i);
        if (!v) { continue; }
        const ContractVerdict* cv = *v;
        if (cv->verdict != Verdict::Agreed) { continue; }
        std::printf("  %-26s %-16s %-9lld %lld\n", cv->merged.symbol,
                    verdict_name(cv->verdict),
                    static_cast<long long>(cv->merged.lot_size.raw()),
                    static_cast<long long>(cv->merged.tick_size.raw()));
        ++shown;
    }
    if (shown == 0) {
        std::printf("  (no agreed contracts to show)\n");
    }
}

void stage_reconcile(Timestamp snap)
{
    rule("[3/4]  P1-06  three-way reconciliation");
    std::printf(
        "  Kite is a CROSS-CHECK, never the authority. Here the exchange\n"
        "  master arrives and agrees about everything except one contract.\n\n");

    const Timestamp exp = ist_date(2026, 9, 24);
    // Agree on everything.
    (void)g_rec.add(nse_row("NIFTY", "NIFTY", 1, 5, 0,
                            OptionType::None, Segment::Cash,
                            Timestamp::epoch(), snap));
    (void)g_rec.add(nse_row("NIFTY", "NIFTY26SEP25000CE", 75, 5, 2'500'000,
                            OptionType::CE, Segment::Opt, exp, snap));
    (void)g_rec.add(nse_row("NIFTY", "NIFTY26SEP25000PE", 75, 5, 2'500'000,
                            OptionType::PE, Segment::Opt, exp, snap));
    (void)g_rec.add(nse_row("NIFTY", "NIFTY26SEPFUT", 75, 5, 0,
                            OptionType::None, Segment::Fut, exp, snap));
    // DISAGREES: the exchange says 30, Kite says 35. One of the two files is
    // stale and nothing here can tell which, so the symbol blocks.
    (void)g_rec.add(nse_row("BANKNIFTY", "BANKNIFTY26SEPFUT", 30, 5, 0,
                            OptionType::None, Segment::Fut, exp, snap));
    // The 25100 strike is deliberately ABSENT from the exchange master, to show
    // the other blocking mode: a primary was loaded this run, so a contract
    // lacking one is a real anomaly rather than today's normal.

    const ReconcileReport r = g_rec.reconcile();
    std::printf("  contracts %zu   agreed %zu   single-source %zu   BLOCKED %zu\n",
                r.contracts, r.agreed, r.single_source, r.blocked);
    std::printf("  primary source seen: %s\n\n",
                r.primary_source_seen ? "yes" : "no");

    std::printf("  %-26s %-16s %-9s %s\n", "contract", "verdict", "lot", "note");
    for (std::size_t i = 0; i < g_rec.size(); ++i) {
        const auto v = g_rec.verdict_at(i);
        if (!v) {
            continue;
        }
        const ContractVerdict* cv = *v;
        const char* note = "";
        if (cv->verdict == Verdict::ValueConflict) {
            note = "<- BLOCKED, sources disagree";
        } else if (cv->verdict == Verdict::MissingPrimary) {
            note = "<- BLOCKED, no exchange row";
        } else if (cv->verdict == Verdict::SingleSource) {
            note = "uncorroborated";
        }
        std::printf("  %-26s %-16s %-9lld %s\n", cv->merged.symbol,
                    verdict_name(cv->verdict),
                    static_cast<long long>(cv->merged.lot_size.raw()), note);
        if (cv->verdict == Verdict::ValueConflict) {
            std::printf("  %-26s   Kite said %lld, the exchange said %lld -- "
                        "neither is guessed at\n", "",
                        static_cast<long long>(
                            cv->values[static_cast<std::size_t>(SpecSource::KiteDump)]),
                        static_cast<long long>(
                            cv->values[static_cast<std::size_t>(SpecSource::NseMaster)]));
        }
    }
}

// ── stage 4 ──────────────────────────────────────────────────────────────
void stage_store()
{
    rule("[4/4]  P1-01  the spec store");

    const auto blocked = apply_to_store(g_rec, g_store);
    if (!blocked.has_value()) {
        std::printf("  store load failed: error %u\n",
                    static_cast<unsigned>(blocked.error()));
        return;
    }
    std::printf("  %zu contracts in the store, %zu blocked\n\n",
                g_store.size(), *blocked);

    std::printf("  A blocked contract is ADDED and then blocked, never dropped:\n"
                "  a tick for it must resolve to Blocked (loud), not NotFound\n"
                "  (which looks like an unknown instrument and gets lost).\n\n");

    std::printf("  %-26s %-12s %s\n", "resolve by Kite token", "token", "result");
    const std::uint32_t probe[] = {11223344u, 12334455u, 99999999u};
    for (std::uint32_t t : probe) {
        const auto id = g_store.id_of(FeedSource::Kite, t);
        if (!id) {
            std::printf("  %-26s %-12u NotFound (no such token)\n", "", t);
            continue;
        }
        const auto spec = g_store.current(*id);
        if (spec) {
            std::printf("  %-26s %-12u ok -> %s, lot %lld\n", "", t,
                        (*spec)->symbol,
                        static_cast<long long>((*spec)->lot_size.raw()));
        } else if (spec.error() == SpecError::Blocked) {
            std::printf("  %-26s %-12u resolves, but reading it says BLOCKED\n",
                        "", t);
        }
    }
    std::printf("\n  spec_version %llu  (part of rule 10's reproducibility tuple)\n",
                static_cast<unsigned long long>(g_store.spec_version()));
}

} // namespace

int run_instruments(const char* path, const char* master_path)
{
    const Timestamp today = ist_date(2026, 8, 31);       // a Monday
    std::printf("\naltair -- Phase 1: the instrument master\n");
    std::printf("session date 2026-08-31 (Monday), IST\n");

    char* owned = nullptr;
    const char* csv = kBuiltinDump;
    std::size_t len = std::strlen(kBuiltinDump);
    const char* origin = "built-in sample (6 contracts)";

    if (path != nullptr) {
        std::size_t n = 0;
        owned = slurp(path, n);
        if (owned == nullptr) {
            std::fprintf(stderr, "altair: cannot read '%s'\n", path);
            return 2;
        }
        // Both real files must be filtered to the SAME universe, or the store
        // fills from whichever is loaded first and the second is refused
        // entirely -- which is exactly what happened the first time this ran
        // against 106'150 Kite rows and 30'488 exchange rows.
        if (master_path != nullptr) {
            n = filter_rows(owned, n, kDemoUniverse);
        }
        csv = owned;
        len = n;
        origin = path;
    }

    stage_snapshot_policy(today);
    const bool ok = stage_parse(csv, len, today, origin);
    if (ok) {
        if (master_path != nullptr) {
            stage_reconcile_real(master_path, today);
        } else {
            stage_reconcile(today);
        }
        stage_store();
    }

    std::free(owned);

    std::printf("\nThat is every Phase 1 card, end to end.\n"
                "Give it a real Kite dump AND a real UDiFF bhavcopy and it\n"
                "reconciles them: two file formats, one contract, one lot size.\n"
                "  altair --instruments <kite.csv> <BhavCopy_NSE_FO_*.csv>\n\n");
    return ok ? 0 : 1;
}

} // namespace altair::demo
