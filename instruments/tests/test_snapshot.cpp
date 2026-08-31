// P1-08a acceptance tests for instruments/snapshot.hpp.
// Plain main() (Catch2 blocked on vcpkg — see LEDGER blocker #7).
//
// No check description in this file may contain the substring FAIL: the
// standing technique is to run the tree under load and grep every line for it.

#include <instruments/snapshot.hpp>
#include <instruments/reconcile.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

namespace {

// ── building IST midnights from civil dates ──────────────────────────────
// Howard Hinnant's days_from_civil. Written out here rather than pulled from
// kite_dump.hpp because that header's copy is in its own detail namespace and
// this card's manifest does not include it.
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

/// The IST midnight of a civil date, as a UTC instant.
constexpr Timestamp ist_date(std::int64_t y, unsigned m, unsigned d) noexcept
{
    return Timestamp{days_from_civil(y, m, d) * 86'400'000'000'000LL
                     - kIstOffset.raw()};
}

// 2026-08-31 is a Monday; 2026-08-28 a Friday. Verified below, not assumed.
constexpr Timestamp kFri = ist_date(2026, 8, 28);
constexpr Timestamp kSat = ist_date(2026, 8, 29);
constexpr Timestamp kMon = ist_date(2026, 8, 31);
constexpr Timestamp kTue = ist_date(2026, 9, 1);
constexpr Timestamp kWed = ist_date(2026, 9, 2);

const MarketCalendar kEmpty{};

// ── 1 ────────────────────────────────────────────────────────────────────
void monday_after_friday_is_one_trading_day()
{
    std::printf("\n1 monday_after_friday_is_one_trading_day\n");

    // Prove the distinction EXISTS before proving it is handled. A test that
    // only asserts the answer cannot tell a correct implementation from one
    // that got the right number for the wrong reason.
    const std::int64_t cal_days =
        (kMon.ns_since_epoch() - kFri.ns_since_epoch()) / 86'400'000'000'000LL;
    check(cal_days == 3, "Friday to Monday is THREE calendar days");

    const std::int32_t td = trading_days_between(kFri, kMon, kEmpty);
    check(td == 1, "but ONE trading day — the weekend does not count");

    SnapshotState s{kFri, true, false};
    s.fetch_ok = false;
    const SnapshotPolicy p{1};
    check(judge_snapshot(s, kMon, kEmpty, p) == SnapshotVerdict::StaleUsable,
          "so a Friday snapshot judged on Monday is StaleUsable, not StaleAging");
    check(!verdict_withholds(judge_snapshot(s, kMon, kEmpty, p)),
          "and it is NOT withheld — a calendar-day rule would refuse it "
          "every Monday");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void holidays_are_excluded()
{
    std::printf("\n2 holidays_are_excluded\n");

    check(trading_days_between(kMon, kWed, kEmpty) == 2,
          "Mon to Wed is 2 trading days with no holidays");

    const Timestamp hol[] = {kTue};
    const MarketCalendar cal{hol, 1, nullptr, 0};
    check(trading_days_between(kMon, kWed, cal) == 1,
          "one holiday in between removes exactly one");

    // A holiday that falls on a weekend must change nothing — it was already
    // not a trading day, and double-counting it would shorten every span that
    // contains one.
    const Timestamp weekend_hol[] = {kSat};
    const MarketCalendar cal2{weekend_hol, 1, nullptr, 0};
    check(trading_days_between(kFri, kMon, cal2) == 1,
          "a holiday ON a Saturday changes nothing — it was never a trading day");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void ist_weekday_not_utc()
{
    std::printf("\n3 ist_weekday_not_utc\n");

    // The hazard, asserted first: an IST midnight is 18:30 UTC the PREVIOUS
    // day. Read the weekday in UTC and it is off by one for every date.
    const std::int64_t utc_day = kMon.ns_since_epoch() / 86'400'000'000'000LL;
    const std::int64_t ist_day = detail::ist_day_number(kMon);
    check(ist_day == utc_day + 1,
          "an IST midnight lands on the PREVIOUS UTC day — the trap exists");
    check(detail::ist_weekday(ist_day) == 0, "and IST calls it a Monday (0)");
    check(detail::ist_weekday(utc_day) == 6,
          "while the naive UTC reading would call it a Sunday");

    check(detail::ist_weekday(detail::ist_day_number(kFri)) == 4, "Friday is 4");
    check(detail::is_weekend(detail::ist_day_number(kSat)), "Saturday is a weekend");
    check(!detail::is_weekend(detail::ist_day_number(kMon)), "Monday is not");

    // 1970-01-01 was a Thursday. Day number 0 must map to 3.
    check(detail::ist_weekday(0) == 3, "day 0 is a Thursday — the epoch anchor");
    // Floor-mod, not truncating %: a pre-1970 date has a negative day number
    // and C++ % would yield a negative weekday.
    check(detail::ist_weekday(-1) >= 0 && detail::ist_weekday(-1) < 7,
          "a negative day number still gives a weekday in 0..6");
    check(detail::ist_weekday(-1) == 2, "the day before the epoch is a Wednesday");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The most important test in the card: the case where the intuitive answer
// (young snapshot, therefore fine) is wrong.
void expiry_crossing_outranks_age()
{
    std::printf("\n4 expiry_crossing_outranks_age\n");

    const Timestamp exp[] = {kMon};
    const MarketCalendar cal{nullptr, 0, exp, 1};
    const SnapshotPolicy p{5};                 // generous — age is NOT the issue

    SnapshotState s{kFri, true, false};
    check(trading_days_between(kFri, kMon, cal) == 1,
          "the snapshot is ONE trading day old — by age alone, fine");
    check(judge_snapshot(s, kMon, kEmpty, p) == SnapshotVerdict::StaleUsable,
          "and with no expiry in the calendar it IS StaleUsable");
    check(judge_snapshot(s, kMon, cal, p) == SnapshotVerdict::ExpiryCrossed,
          "but with an expiry between, it is ExpiryCrossed — age is outranked");
    check(verdict_withholds(judge_snapshot(s, kMon, cal, p)),
          "so the source is withheld, even though it is only a day old");

    // And the converse: an old snapshot mid-cycle is NOT withheld.
    SnapshotState old_s{ist_date(2026, 8, 25), true, false};
    check(judge_snapshot(old_s, kMon, kEmpty, p) == SnapshotVerdict::StaleAging,
          "a 4-trading-day-old snapshot mid-cycle is only StaleAging");
    check(!verdict_withholds(judge_snapshot(old_s, kMon, kEmpty, p)),
          "and is NOT withheld — lot sizes do not change on a Tuesday");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void expiry_interval_bounds()
{
    std::printf("\n5 expiry_interval_bounds\n");

    // The lower bound is CLOSED, correcting the half-open form the card first
    // specified. A snapshot stamped with trading date D is fetched at 08:15 on
    // D — before that day's 15:30 expiry — so it is a PRE-rollover file. Used
    // on D+1 it would size every derivative from the previous cycle's lot
    // table, which is rule 1's failure mode.
    const Timestamp on_snapshot[] = {kFri};
    const MarketCalendar c1{nullptr, 0, on_snapshot, 1};
    check(expiry_crossed(kFri, kMon, c1),
          "an expiry ON the snapshot date IS crossed once we are past that day "
          "— that file is pre-rollover and cannot be known to be otherwise");

    const Timestamp on_now[] = {kMon};
    const MarketCalendar c2{nullptr, 0, on_now, 1};
    check(expiry_crossed(kFri, kMon, c2), "an expiry today HAS been crossed");

    // The guard keeps the same day safe: this morning's own snapshot is fine
    // on the day it was taken, expiry or not.
    check(!expiry_crossed(kFri, kFri, c1),
          "but on the day itself it is NOT crossed — today's own snapshot is "
          "usable on an expiry day");
    check(!expiry_crossed(kMon, kMon, c2), "a zero-length span crosses nothing");
    check(!expiry_crossed(kMon, kFri, c2),
          "and a backwards span crosses nothing rather than reporting the "
          "expiry twice");

    // An expiry strictly outside the span is not crossed, so the closed bound
    // has not simply made everything true.
    const Timestamp earlier[] = {ist_date(2026, 8, 27)};
    const MarketCalendar c3{nullptr, 0, earlier, 1};
    check(!expiry_crossed(kFri, kMon, c3),
          "an expiry BEFORE the snapshot date is not crossed — the closed "
          "bound did not just make everything true");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// Proves the composition claimed in the card, rather than asserting it in a
// comment: a withheld source needs NO new blocking machinery.
Reconciler g_rec;

ContractSpec make(SpecSource src, const char* underlying, std::uint32_t token)
{
    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.lot_size = LotSize{75};
    s.tick_size = Price{5};
    s.strike = Price{2'500'000};
    s.price_scale = 100;
    s.expiry = ist_date(2026, 9, 24);
    s.valid_from = kMon;
    s.valid_to = Timestamp::max();
    s.source_hash = 0xA000u + static_cast<std::uint64_t>(src);
    s.snapshot_at = kMon;
    s.exchange = Exchange::NSE;
    s.segment = Segment::Opt;
    s.opt_type = OptionType::CE;
    s.source = src;
    std::snprintf(s.symbol, sizeof(s.symbol), "%s25SEP", underlying);
    std::snprintf(s.underlying, sizeof(s.underlying), "%s", underlying);
    if (src == SpecSource::KiteDump) {
        s.token[static_cast<std::size_t>(FeedSource::Kite)] = token;
    }
    return s;
}

void withheld_verdicts_block_through_p1_06()
{
    std::printf("\n6 withheld_verdicts_block_through_p1_06\n");

    const Timestamp exp[] = {kMon};
    const MarketCalendar cal{nullptr, 0, exp, 1};
    const SnapshotState nse{kFri, true, false};
    const SnapshotVerdict v = judge_snapshot(nse, kMon, cal, SnapshotPolicy{5});
    check(verdict_withholds(v), "the NSE master is withheld this session");

    // Withholding IS simply not calling add(). No new API, no second flag.
    g_rec.clear();
    if (!verdict_withholds(v)) {
        (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 0));
    }
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 11111));

    // A primary WAS loaded somewhere this run, so the missing one is an anomaly.
    (void)g_rec.add(make(SpecSource::NseMaster, "BANKNIFTY", 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "BANKNIFTY", 22222));

    const ReconcileReport r = g_rec.reconcile();
    check(r.primary_source_seen, "a primary was seen for the un-withheld symbol");
    check(r.missing_primary == 1, "and the withheld one is MissingPrimary");
    check(r.blocked == 1, "which blocks it");

    for (std::size_t i = 0; i < g_rec.size(); ++i) {
        const ContractVerdict* cv = *g_rec.verdict_at(i);
        if (std::strncmp(cv->merged.underlying, "NIFTY", 6) == 0) {
            check(cv->verdict == Verdict::MissingPrimary,
                  "NIFTY, whose primary was withheld, is the blocked one");
            check(verdict_blocks(cv->verdict),
                  "through P1-06's EXISTING machinery — no second blocking path");
        }
    }
}

// ── 7 ────────────────────────────────────────────────────────────────────
void no_snapshot_halts()
{
    std::printf("\n7 no_snapshot_halts\n");

    const SnapshotState none{};
    const SnapshotVerdict v = judge_snapshot(none, kMon, kEmpty, SnapshotPolicy{1});
    check(v == SnapshotVerdict::NoSnapshot, "nothing on disk is NoSnapshot");
    check(verdict_halts(v), "and it halts the session");
    check(!verdict_withholds(v),
          "but it does NOT merely withhold — halting and withholding are "
          "different outcomes and must not be conflated");
    check(!verdict_halts(SnapshotVerdict::TooOld),
          "and conversely TooOld withholds without halting");
    check(verdict_withholds(SnapshotVerdict::TooOld), "TooOld withholds");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void future_snapshot_is_too_old()
{
    std::printf("\n8 future_snapshot_is_too_old\n");

    const SnapshotState ahead{kWed, true, false};      // dated after `now`
    const std::int32_t age = trading_days_between(kWed, kMon, kEmpty);
    check(age < 0, "the age is negative — the trap the naive reading falls into");
    check(age == -2, "two trading days into the future");

    const SnapshotVerdict v = judge_snapshot(ahead, kMon, kEmpty, SnapshotPolicy{5});
    check(v == SnapshotVerdict::TooOld,
          "a snapshot dated in the future is TooOld, not fresh");
    check(verdict_withholds(v), "and is withheld");
}

// ── policy edges ─────────────────────────────────────────────────────────
void policy_edges()
{
    std::printf("\npolicy edges\n");

    const SnapshotState fri{kFri, true, false};

    check(judge_snapshot(fri, kMon, kEmpty, SnapshotPolicy{0})
              == SnapshotVerdict::TooOld,
          "max_stale_days == 0 means ONLY today — not unlimited");

    SnapshotState today{kMon, true, false};
    check(judge_snapshot(today, kMon, kEmpty, SnapshotPolicy{0})
              == SnapshotVerdict::StaleUsable,
          "and today's snapshot still passes under that policy");

    today.fetch_ok = true;
    check(judge_snapshot(today, kMon, kEmpty, SnapshotPolicy{0})
              == SnapshotVerdict::Fresh,
          "a successful fetch is Fresh");
    check(!stale_flag_for(SnapshotVerdict::Fresh), "Fresh is not stale");
    check(stale_flag_for(SnapshotVerdict::StaleUsable), "StaleUsable is stale");
    check(stale_flag_for(SnapshotVerdict::ExpiryCrossed), "so is ExpiryCrossed");

    // A very old snapshot must not spin the day loop.
    const SnapshotState ancient{ist_date(1995, 1, 2), true, false};
    check(judge_snapshot(ancient, kMon, kEmpty, SnapshotPolicy{5})
              == SnapshotVerdict::TooOld,
          "a 30-year-old snapshot is TooOld and the span loop saturates");
}

} // namespace

int main()
{
    std::printf("altair instruments snapshot tests\n");
    monday_after_friday_is_one_trading_day();
    holidays_are_excluded();
    ist_weekday_not_utc();
    expiry_crossing_outranks_age();
    expiry_interval_bounds();
    withheld_verdicts_block_through_p1_06();
    no_snapshot_halts();
    future_snapshot_is_too_old();
    policy_edges();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
