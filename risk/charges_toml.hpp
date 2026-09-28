// risk/charges_toml.hpp -- config/charges.toml -> ChargeSchedule.
//
// P3-09b. A CORRECTION CARD, and the defect is that this file did not exist.
//
// P3-09 shipped as "Cost calculator -- full stack, effective-dated
// charges.toml" and the calculator is right: itemised, 128-bit intermediates,
// premium-vs-notional carried in the type, side conventions checked, 405 lines
// of test. What it never had was a way to READ charges.toml.
//
// ROADMAP §9, at the line that specifies this: the rates "live in
// config/charges.toml, never in code". They were in code. The only place the
// real numbers existed was `risk/tests/test_cost.cpp`, which mirrors the file
// by hand and says so in its own header comment -- so the test and the config
// agreed exactly as long as somebody kept editing both, and nothing checked.
//
// Nothing loaded the file because nothing COULD. `core/config/toml_source.hpp`
// flattens a document into a ConfigSnapshot of int64/double/bool riding a
// seqlock, and charges.toml is two `[[schedule]]` blocks -- an array of
// tables, which that walker counts as `skipped_other` and never turns into
// keys, deliberately and correctly for what it is. Verified by reading the
// walker, not assumed: `toml_source.cpp` line 119.
//
// So this is a SEPARATE, COLD reader for a shape the snapshot cannot hold.
//
// WHY THIS MATTERS MORE THAN IT LOOKS.
//
// Rule 5: every signal is priced net of full cost before it exists. A cost
// calculator with no schedule source is a rule-5 hole with a passing test in
// front of it. And CLAUDE.md's reality check -- "STT rose on 2026-04-01, every
// pre-April backtest is optimistic until re-run" -- is only actionable if the
// backtester can reach the old schedule, which requires both to be loaded from
// a file that is effective-dated rather than one compiled-in rate.
//
// A MISSING RATE BLOCKS. IT DOES NOT DEFAULT TO ZERO.
//
// This is the whole safety argument for the file. A charge that fails to parse
// and silently becomes 0 makes every strategy look more profitable, which is
// the direction nobody investigates. Rule 9: ambiguity blocks and raises. So a
// segment table that is present but unreadable is an error, and a segment
// table that is ABSENT sets `present = false` -- which `compute_cost` already
// turns into `UnknownSegment` rather than a free trade.
//
// AND `last_verified = "UNVERIFIED"` BLOCKS PRICING.
//
// Every schedule loaded from the file carries its verification state, and
// `compute_cost` refuses an unverified schedule. The required safety flag must
// also be present as a boolean true; absent, malformed, or disabled policy is
// a loader error. The other keys under `[safety]`, broker tables and implicit
// seeds are not enforced by this loader/card.

#pragma once

#include "cost.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class ChargesError : std::uint8_t {
    FileNotFound,
    FileTooLarge,
    ParseFailed,
    /// A `[[schedule]]` block with no `valid_from`/`valid_to`, or dates that
    /// do not parse. An undated schedule cannot be selected by trade date and
    /// is refused rather than being given a range.
    BadDateRange,
    /// A rate that is present but not a number, or negative. Refused, never
    /// treated as zero -- see the header.
    BadRate,
    /// A side string that is not buy/sell/both/none, or a turnover_basis that
    /// is not notional/premium. Refused for the same reason.
    BadEnum,
    /// No `[[schedule]]` blocks at all.
    NoSchedules,
    /// Two schedules cover the same instant. The calculator picks the first
    /// match, so an overlap silently decides which rates a backtest gets.
    OverlappingSchedules,
    /// `[safety].block_on_unverified_schedule` must be explicitly boolean true.
    UnsafeVerificationPolicy
};

/// Largest charges file accepted. UNIT: bytes.
inline constexpr std::size_t kMaxChargesBytes = 1u << 18;   // 256 KiB

struct ChargesLoadReport {
    std::size_t schedules = 0;
    std::size_t segment_tables = 0;
    /// True only when `last_verified` is a valid ISO calendar date and
    /// `verified_by` is non-blank. Copied into every schedule's `verified`.
    bool verified = false;
    /// The gap, in days, between the last schedule's `valid_to` and the end of
    /// time. Zero means the file covers the future; a positive number means
    /// trades after that date have NO schedule and will be refused.
    std::int64_t uncovered_after_days = 0;
};

/// Parse charges.toml into effective-dated schedules, ascending by valid_from.
///
/// `out` is CLEARED first. This is not a merge: two files' worth of charge
/// schedules silently overlapping is exactly the OverlappingSchedules case
/// this refuses within one file.
[[nodiscard]] std::expected<ChargesLoadReport, ChargesError>
load_charges(const char* text, std::size_t len,
             std::vector<ChargeSchedule>& out) noexcept;

[[nodiscard]] std::expected<ChargesLoadReport, ChargesError>
load_charges_file(const char* path,
                  std::vector<ChargeSchedule>& out) noexcept;

/// A human-readable name for an error, for a message that says what to fix.
[[nodiscard]] const char* charges_error_text(ChargesError e) noexcept;

} // namespace altair
