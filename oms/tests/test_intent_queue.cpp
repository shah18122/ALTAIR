// CX02-B2 / B2b acceptance tests for oms/intent_queue.hpp.
//
// Finding C13-008: the P25-03 drain treated a missing file as an empty queue,
// trusted an offset into a file that could have been truncated or replaced,
// never expired an intent, never deduplicated one, and had no bound on batch
// or line length. Tests 1-6 cover those.
//
// Tests 7-9 are CX02-B2b, from the independent review: per-kind refusal counts
// that survive a capped list (R-AB-007), a rollback after a FAILED state save
// so un-acted requests do not come back as duplicates (R-AB-008), a policy
// that refuses its own livelock (R-AB-010), and the 1,024-byte line with both
// endings (R-AB-011).
//
// Files are written under the working directory with a per-run prefix and
// removed at the end. Nothing here touches data/order_intents.jsonl.
//
// No check description here may contain the substring FAIL.

#include <oms/intent_queue.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair::oms;
namespace fs = std::filesystem;

/// 2026-09-08T18:22:31+05:30, as UTC ns (checked in test_order_intent.cpp).
constexpr std::int64_t kAt = 1'788'871'951'000'000'000LL;
constexpr std::int64_t kSec = 1'000'000'000LL;

std::string prefix;

std::string file(const char* name) { return prefix + name; }

/// A drainer, or a check failure and a default-policy one.
IntentDrainer make(DrainPolicy p = {}) {
    auto d = IntentDrainer::create(p);
    if (!d) {
        check(false, "(a drainer was created)");
        return *IntentDrainer::create();
    }
    return std::move(*d);
}

std::string intent(const std::string& id,
                   const char* at = "2026-09-08T18:22:31+05:30") {
    return std::string(R"({"v":1,"id":")") + id + R"(","at":")" + at
         + R"(","by":"smit","token":260105,"symbol":"NIFTY BANK","exchange":"NSE","side":"BUY","lots":1,"order_type":"LIMIT","limit_paise":5711490,"product":"NRML","validity":"DAY"})";
}

void write(const std::string& path, const std::string& bytes,
           bool append = false) {
    std::ofstream f(path, std::ios::binary
                              | (append ? std::ios::app : std::ios::trunc));
    f << bytes;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void missing_file_is_an_error_not_an_empty_queue() {
    std::printf("\n1 missing_file_is_an_error_not_an_empty_queue\n");
    IntentDrainer d = make();
    const auto r = d.drain(file("does_not_exist.jsonl"), {}, kAt);
    check(!r && r.error() == QueueError::Unreadable,
          "a missing queue file is Unreadable -- the old drain returned zero"
          " intents, the same answer as nothing new");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void truncated_and_replaced_files_are_refused() {
    std::printf("\n2 truncated_and_replaced_files_are_refused\n");
    const std::string q = file("trunc.jsonl");
    write(q, intent("A1") + "\n" + intent("A2") + "\n");
    IntentDrainer d = make();
    const auto first = d.drain(q, {}, kAt + kSec);
    check(first && first->accepted.size() == 2, "two intents drain");
    if (!first) { return; }
    const IntentCursor cur = first->next;

    write(q, intent("A1") + "\n");                     // shorter now
    const auto t = d.drain(q, cur, kAt + kSec);
    check(!t && t.error() == QueueError::Truncated,
          "a file shorter than the cursor is Truncated, not silently empty");

    // Same length, different bytes before the cursor.
    write(q, intent("B1") + "\n" + intent("B2") + "\n");
    const auto rep = d.drain(q, cur, kAt + kSec);
    check(!rep && rep.error() == QueueError::Replaced,
          "a replaced file of the same size is Replaced -- the old drain would"
          " have resumed at a byte offset into somebody else's records");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void stale_and_future_intents_are_refused() {
    std::printf("\n3 stale_and_future_intents_are_refused\n");
    const std::string q = file("age.jsonl");
    write(q, intent("OLD1") + "\n");
    IntentDrainer d = make();
    const auto old = d.drain(q, {}, kAt + 31 * kSec);
    check(old && old->accepted.empty() && old->refused.expired == 1
              && old->quarantined.size() == 1
              && old->quarantined[0].kind == QuarantineKind::Expired,
          "an intent 31 s old is EXPIRED -- nothing used to expire, so a"
          " drainer started days later would act on it");
    check(old && old->next.offset > 0,
          "and it is consumed, so it is not re-judged forever");

    IntentDrainer e = make();
    const auto fresh = e.drain(q, {}, kAt + 29 * kSec);
    check(fresh && fresh->accepted.size() == 1, "29 s old is still accepted");

    IntentDrainer g = make();
    const auto future = g.drain(q, {}, kAt - 6 * kSec);
    check(future && future->accepted.empty()
              && future->refused.future_dated == 1,
          "an intent dated 6 s in the future is refused as mis-stamped");

    const auto impossible_clock = g.drain(
        q, {}, std::numeric_limits<std::int64_t>::min());
    check(!impossible_clock && impossible_clock.error() == QueueError::BadClock,
          "an out-of-range clock is refused before TTL arithmetic can overflow");

    // Once the old request has aged out of the remembered window, its ID may
    // be reused by a newly timestamped request without waiting for table-full
    // pressure.
    DrainPolicy short_ttl;
    short_ttl.ttl_ns = 30 * kSec;
    auto reuse_made = IntentDrainer::create(short_ttl);
    check(reuse_made.has_value(), "a short TTL policy is valid");
    if (reuse_made) {
        IntentDrainer reuse = std::move(*reuse_made);
        write(q, intent("REUSED") + "\n");
        const auto first = reuse.drain(q, {}, kAt + kSec);
        check(first && first->accepted.size() == 1,
              "the first request with this id is accepted");
        write(q, intent("REUSED", "2026-09-08T18:23:11+05:30") + "\n");
        const auto again = reuse.drain(q, {}, kAt + 40 * kSec);
        check(again && again->accepted.size() == 1,
              "the same id is reusable once its remembered request expires");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
void redrain_after_crash_does_not_duplicate() {
    std::printf("\n4 redrain_after_crash_does_not_duplicate\n");
    const std::string q = file("crash.jsonl");
    const std::string st = file("crash.state");
    write(q, intent("C1") + "\n" + intent("C2") + "\n");

    IntentDrainer a = make();
    const auto b1 = a.drain(q, {}, kAt + kSec);
    check(b1 && b1->accepted.size() == 2, "two intents drain");
    if (!b1) { return; }
    // PERSIST, THEN ACT -- the contract. Then the process dies.
    check(save_drainer_state(st, b1->next, a).has_value(),
          "state is saved before acting");

    IntentDrainer b = make();
    const auto cur = load_drainer_state(st, b);
    check(cur.has_value() && cur->offset == b1->next.offset,
          "a restarted drainer loads the cursor");
    if (!cur) { return; }
    const auto again = b.drain(q, *cur, kAt + 2 * kSec);
    check(again && again->accepted.empty(),
          "and from it sees nothing new");

    // Worse: the cursor is lost and the queue is read from the start.
    const auto from_zero = b.drain(q, {}, kAt + 2 * kSec);
    check(from_zero && from_zero->accepted.empty()
              && from_zero->refused.duplicate_ids == 2,
          "even from offset 0 both ids are refused as duplicates -- the ids"
          " were saved with the cursor");

    write(st, "altair-intent-cursor 1\noffset 10\ntail 5\nid C1 1\n");
    IntentDrainer c = make();
    const auto torn = load_drainer_state(st, c);
    check(!torn && torn.error() == QueueError::CorruptState,
          "a state file with no end line is CorruptState, not a fresh start");
    write(st, "altair-intent-cursor 1\noffset 0\ntail 5\nid C1 1788871951000000000\nend\njunk\n");
    IntentDrainer trailing = make();
    const auto trailing_state = load_drainer_state(st, trailing);
    check(!trailing_state && trailing_state.error() == QueueError::CorruptState
              && trailing.remembered().empty(),
          "state with trailing data is rejected without partially restoring ids");
    write(st, "altair-intent-cursor 1\noffset 0\ntail 5\nid C1 1788871951000000000\nid C1 1788871951000000000\nend\n");
    IntentDrainer repeated = make();
    const auto repeated_state = load_drainer_state(st, repeated);
    check(!repeated_state && repeated_state.error() == QueueError::CorruptState
              && repeated.remembered().empty(),
          "duplicate remembered IDs are corrupt and restore transactionally");
    write(st, "altair-intent-cursor 1\noffset 0\ntail 5\nid C1 "
             + std::to_string(std::numeric_limits<std::int64_t>::max())
             + "\nend\n");
    IntentDrainer impossible_timestamp = make();
    const auto impossible_state = load_drainer_state(st, impossible_timestamp);
    check(!impossible_state
              && impossible_state.error() == QueueError::CorruptState
              && impossible_timestamp.remembered().empty(),
          "an impossible dedupe timestamp is corrupt, not a never-expiring ID");
    IntentDrainer n = make();
    const auto none = load_drainer_state(file("no.state"), n);
    check(!none && none.error() == QueueError::NoState,
          "and a missing one is NoState, which the caller can tell apart");

    // R-AB-009: a state path that EXISTS but cannot be read is not "no state"
    // either. A directory is the case that is deterministic on every
    // platform: `exists` says yes, the open fails.
    //
    // The other half of R-AB-009 -- `exists()` itself failing and setting an
    // error_code (a permission error, an unavailable share) -- cannot be
    // forced portably from a test: MSVC maps a path under a file to "not
    // found" with no error set. That branch is covered by inspection
    // (intent_queue.hpp, load_drainer_state), not by this test, and saying so
    // is better than a check that passes for the wrong reason.
    const std::string as_dir = file("as_dir.state");
    std::error_code mk;
    fs::create_directories(as_dir, mk);
    IntentDrainer u = make();
    const auto bad_path = load_drainer_state(as_dir, u);
    check(!bad_path && bad_path.error() != QueueError::NoState,
          "a state path that exists and cannot be read is NOT reported as"
          " absent -- starting over would forget every remembered id");
    fs::remove_all(as_dir, mk);
}

// ── 5 ────────────────────────────────────────────────────────────────────
void bounds_report_rather_than_drop() {
    std::printf("\n5 bounds_report_rather_than_drop\n");
    const std::string q = file("batch.jsonl");
    write(q, intent("D1") + "\n" + intent("D2") + "\n" + intent("D3") + "\n");
    DrainPolicy p;
    p.max_batch = 2;
    IntentDrainer d = make(p);
    const auto b1 = d.drain(q, {}, kAt + kSec);
    check(b1 && b1->accepted.size() == 2 && b1->more,
          "max_batch 2 of 3: two accepted and `more` set");
    if (!b1) { return; }
    const auto b2 = d.drain(q, b1->next, kAt + kSec);
    check(b2 && b2->accepted.size() == 1 && b2->accepted[0].id == "D3"
              && !b2->more,
          "the third is left for the next call, not dropped");

    // max_lines stops a call the same way, counting blank and refused lines.
    DrainPolicy lp;
    lp.max_lines = 2;
    IntentDrainer l = make(lp);
    const auto lb = l.drain(q, {}, kAt + kSec);
    check(lb && lb->lines == 2 && lb->more && lb->accepted.size() == 2,
          "max_lines 2 stops after two lines, with `more` set");

    const std::string big = file("big.jsonl");
    write(big, std::string(100'000, 'x') + "\n" + intent("E1") + "\n");
    IntentDrainer e = make();
    const auto r = e.drain(big, {}, kAt + kSec);
    check(r && r->refused.too_long == 1 && r->quarantined.size() == 1
              && r->quarantined[0].kind == QuarantineKind::TooLong
              && r->accepted.size() == 1 && r->accepted[0].id == "E1",
          "a 100 KB line is quarantined as TooLong and the next line survives");

    const std::string bad = file("bad.jsonl");
    write(bad, "{\n{\n{\n{\n{\n");
    DrainPolicy small;
    small.max_quarantine_list = 2;
    IntentDrainer b = make(small);
    const auto qr = b.drain(bad, {}, kAt + kSec);
    check(qr && qr->quarantined.size() == 2 && qr->refused.unparseable == 5
              && qr->refused.total() == 5,
          "the quarantine LIST is capped at 2 while the per-kind counts still"
          " say five lines were unparseable");

    const std::string many = file("dedupe.jsonl");
    write(many, intent("F1") + "\n" + intent("F2") + "\n" + intent("F3") + "\n");
    DrainPolicy tiny;
    tiny.dedupe_capacity = 2;
    IntentDrainer t = make(tiny);
    const auto full = t.drain(many, {}, kAt + kSec);
    check(full && full->accepted.size() == 2 && full->refused.dedupe_full == 1,
          "a full dedupe table REFUSES a new id rather than forgetting a live"
          " one");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void torn_tail_then_valid_line_quarantines_one_accepts_one() {
    std::printf("\n6 torn_tail_then_valid_line_quarantines_one_accepts_one\n");
    const std::string q = file("torn.jsonl");
    const std::string whole = intent("G1");
    write(q, whole.substr(0, 120));                    // writer died
    IntentDrainer d = make();
    const auto b1 = d.drain(q, {}, kAt + kSec);
    check(b1 && b1->accepted.empty() && b1->partial_tail
              && b1->next.offset == 0,
          "a torn tail is left unconsumed and reported as a partial tail");
    if (!b1) { return; }
    // CX02-B3's writer starts a new line when the file does not end in one.
    write(q, "\n" + intent("G2") + "\r\n", true);
    const auto b2 = d.drain(q, b1->next, kAt + kSec);
    check(b2 && b2->refused.unparseable == 1 && b2->refused.total() == 1
              && b2->accepted.size() == 1 && b2->accepted[0].id == "G2",
          "the torn record is quarantined on its own line and the next request"
          " -- CRLF and all -- is accepted intact");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// R-AB-008. The batch was never acted on, because the save failed.
void failed_save_does_not_burn_ids() {
    std::printf("\n7 failed_save_does_not_burn_ids\n");
    const std::string q = file("rollback.jsonl");
    write(q, intent("H1") + "\n");
    IntentDrainer d = make();
    const auto b1 = d.drain(q, {}, kAt + kSec);
    check(b1 && b1->accepted.size() == 1, "one intent drains");
    if (!b1) { return; }

    // The state save fails: its directory does not exist.
    const auto saved = save_drainer_state(file("no_such_dir/x.state"),
                                          b1->next, d);
    check(!saved && saved.error() == QueueError::StateWriteFailed,
          "saving the cursor fails, so the caller must not act on the batch");

    d.rollback(*b1);
    const auto b2 = d.drain(q, {}, kAt + 2 * kSec);
    check(b2 && b2->accepted.size() == 1 && b2->accepted[0].id == "H1"
              && b2->refused.duplicate_ids == 0,
          "after rollback the same request drains again -- without it, a"
          " request nobody acted on came back as a DuplicateId and was never"
          " executed");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// R-AB-010. A policy that would consume nothing for ever is refused.
void a_livelocking_policy_is_refused() {
    std::printf("\n8 a_livelocking_policy_is_refused\n");
    DrainPolicy zero_batch;
    zero_batch.max_batch = 0;
    const auto a = IntentDrainer::create(zero_batch);
    check(!a && a.error() == QueueError::BadPolicy,
          "max_batch 0 is refused: every call would return `more` and consume"
          " nothing");
    DrainPolicy zero_lines;
    zero_lines.max_lines = 0;
    check(!IntentDrainer::create(zero_lines), "max_lines 0 is refused");
    DrainPolicy neg_ttl;
    neg_ttl.ttl_ns = -1;
    check(!IntentDrainer::create(neg_ttl),
          "a negative TTL is refused: it would expire every intent");
    DrainPolicy no_dedupe;
    no_dedupe.dedupe_capacity = 0;
    check(!IntentDrainer::create(no_dedupe),
          "a zero dedupe capacity is refused: it would refuse every intent");
    check(IntentDrainer::create().has_value(), "the default policy is valid");
}

// ── 9 ────────────────────────────────────────────────────────────────────
// R-AB-011. The line-length bound must not depend on the line ending.
void the_length_bound_does_not_count_the_carriage_return() {
    std::printf("\n9 the_length_bound_does_not_count_the_carriage_return\n");
    const std::string base = intent("I1");
    // Pad with JSON whitespace between `{` and the first key to exactly the
    // bound. The grammar skips it; the byte counter must not.
    const std::size_t pad = kMaxIntentLineBytes - base.size();
    const std::string at_cap =
        base.substr(0, 1) + std::string(pad, ' ') + base.substr(1);
    check(at_cap.size() == kMaxIntentLineBytes, "(the line is exactly at the bound)");

    const std::string lf = file("cap_lf.jsonl");
    write(lf, at_cap + "\n");
    IntentDrainer a = make();
    const auto ra = a.drain(lf, {}, kAt + kSec);
    check(ra && ra->accepted.size() == 1,
          "a line of exactly kMaxIntentLineBytes with LF is accepted");

    const std::string crlf = file("cap_crlf.jsonl");
    write(crlf, at_cap + "\r\n");
    IntentDrainer b = make();
    const auto rb = b.drain(crlf, {}, kAt + kSec);
    check(rb && rb->accepted.size() == 1 && rb->refused.too_long == 0,
          "and so is the same line with CRLF -- the carriage return used to"
          " count toward the bound and quarantine it as TooLong");

    const std::string over = file("over.jsonl");
    write(over, at_cap + " \r\n");
    IntentDrainer c = make();
    const auto rc = c.drain(over, {}, kAt + kSec);
    check(rc && rc->refused.too_long == 1,
          "one byte past the bound is TooLong with either ending");
}

} // namespace

int main() {
    std::printf("CX02-B2/B2b -- draining the intent queue\n");
    prefix = "cx02_iq_"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()) + "_";

    missing_file_is_an_error_not_an_empty_queue();
    truncated_and_replaced_files_are_refused();
    stale_and_future_intents_are_refused();
    redrain_after_crash_does_not_duplicate();
    bounds_report_rather_than_drop();
    torn_tail_then_valid_line_quarantines_one_accepts_one();
    failed_save_does_not_burn_ids();
    a_livelocking_policy_is_refused();
    the_length_bound_does_not_count_the_carriage_return();

    std::error_code ec;
    for (const char* n : {"trunc.jsonl", "age.jsonl", "crash.jsonl", "crash.state",
                          "batch.jsonl", "big.jsonl", "bad.jsonl", "dedupe.jsonl",
                          "torn.jsonl", "rollback.jsonl", "cap_lf.jsonl",
                          "cap_crlf.jsonl", "over.jsonl"}) {
        fs::remove(file(n), ec);
    }
    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
