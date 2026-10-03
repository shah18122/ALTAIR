// The execution labels on a market whose answers are known: shortfall
// against the decision's mid, the spread then, markouts read at exactly
// fill + h, a passive order filled by the queue ahead being traded through,
// horizons past the tape's end left unknown, and a fill from before the tape
// given no decision-time label at all.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/exec_study.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>
#include <server/quote_payload.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair;
using namespace altair::live;
namespace ex = altair::live::execution;

constexpr std::int64_t kSec = 1'000'000'000LL;
const std::int64_t kDay = parse_day("2026-09-24");
const std::int64_t kT0 = (kDay * 86400 + 10 * 3600 - 19800) * kSec;   // 10:00 IST: the decision

LiveInstrument inst(std::uint32_t tok, const char* sym, LiveKind k, std::int64_t lot) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = std::string("NSE:") + sym; i.underlying = "NIFTY"; i.kind = k; i.lot = lot;
    i.expiry_day = kDay + 30; i.tick = 0.05;
    return i;
}

struct Tape {
    ex::ExecStudy& study;
    std::uint64_t trade_seq = 0, quote_seq = 0;
    std::uint8_t body[512]{};
    void quote(std::int64_t at, std::int64_t bid, std::int64_t ask, std::int64_t qty) {
        FrameHeader h;
        h.kind = FrameKind::Delta; h.channel = Channel::State; h.topic = kTopicQuote; h.seq = ++quote_seq; h.engine_time_ns = at;
        QuotePayload q; q.token = 10; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = qty; q.ask_qty = qty;
        h.payload_len = static_cast<std::uint32_t>(*encode_quote(q, body, sizeof body));
        study.on_frame(h, body);
    }
    void trade(std::int64_t at, std::int64_t px, std::int64_t qty, std::uint32_t tok = 10) {
        FrameHeader h;
        h.kind = FrameKind::Delta; h.channel = Channel::State; h.topic = kTopicTrades; h.seq = ++trade_seq; h.engine_time_ns = at;
        PricePayload p; p.token = tok; p.last_paise = px; p.last_qty = qty; p.exchange_ts_ns = at;
        h.payload_len = static_cast<std::uint32_t>(*encode_price(p, nullptr, nullptr, body, sizeof body));
        study.on_frame(h, body);
    }
};

report::JournalFill fill(std::int64_t ns, std::int64_t submit, int side, bool open, double px, std::int64_t qty) {
    report::JournalFill f;
    f.ns = ns; f.submit_ns = submit; f.model = "m"; f.token = 10; f.symbol = "FUT_A"; f.open = open; f.side = side; f.qty = qty;
    f.price = px; f.expenses = 1.0;
    return f;
}
bool close_to(double a, double b, double tol = 1e-6) { return std::fabs(a - b) < tol; }

} // namespace

int main() {
    std::printf("live execution study\n");
    const std::vector<LiveInstrument> u{inst(kLiveNiftyToken, "NIFTY 50", LiveKind::Index, 1), inst(10, "FUT_A", LiveKind::Future, 50)};
    const std::vector<report::JournalFill> fills{
        fill(kT0 + kSec / 4, kT0, 1, true, 24001.0, 50),             // a buy at the ask, 250 ms after deciding at 10:00:00
        fill(kT0 + 65 * kSec, kT0 + 65 * kSec, -1, false, 24008.0, 50),   // the exit, five seconds before the tape ends
        fill(kT0 - 600 * kSec, kT0 - 600 * kSec, 1, true, 23900.0, 50),   // another session's, before this tape began
    };
    ex::ExecStudy study(u, fills);
    Tape t{study};
    t.trade(kT0 - 2 * kSec, 2400000, 10);
    t.quote(kT0 - kSec, 2399900, 2400100, 100);     // the decision's market: 23999 / 24001, 100 each side
    t.quote(kT0 + kSec / 2, 2400400, 2400600, 100);
    t.quote(kT0 + kSec + kSec / 10, 2400900, 2401100, 100);   // mid 24010 from 10:00:01.1
    t.quote(kT0 + kSec + kSec / 2, 2401900, 2402100, 100);    // mid 24020 from 10:00:01.5: after fill + 1 s
    t.trade(kT0 + 2 * kSec, 2399900, 80);     // at the bid the buy would have joined: 80 of the 150 it needed
    t.trade(kT0 + 3 * kSec, 2399900, 80);     // 160: the queue ahead (100) and the order (50) traded through
    for (int s = 4; s <= 70; ++s) t.trade(kT0 + s * kSec, 2400800, 1);   // the tape runs to 10:01:10
    const auto labels = study.finish();
    check(labels.size() == 3, "a label per fill");
    const auto& a = labels[0];
    check(close_to(a.decision_mid, 24000.0) && close_to(a.shortfall_bp, 1.0 / 24000.0 * 1e4),
          "shortfall: a rupee over the decision's mid, in bp");
    check(close_to(a.decision_spread_bp, 2.0 / 24000.0 * 1e4), "and the spread at the decision");
    check(close_to(a.markout_bp[0], (24010.0 - 24001.0) / 24001.0 * 1e4),
          "the 1 s markout reads the market as of fill + 1 s exactly, not the next quote after it");
    check(std::isnan(a.markout_bp[4]), "a 300 s markout past the tape's end is unknown, not zero");
    check(a.queue_ahead == 100 && close_to(a.passive_price, 23999.0), "a passive buy would have joined 100 at the bid");
    check(a.passive_filled[0] == 0 && a.passive_filled[1] == 1 && a.passive_filled[3] == 1,
          "filled 3 s later, when the queue ahead and the order had traded: not within 1 s, within 5 and 60");
    const auto& b = labels[1];
    check(b.passive_filled[0] == 0 && b.passive_filled[2] == -1, "a passive sell nobody traded with: 0 within 1 s, unknown past the tape");
    check(close_to(b.shortfall_bp, -1.0 * (24008.0 - 24020.0) / 24020.0 * 1e4), "a sell's shortfall is signed for the sell");
    const auto& c = labels[2];
    check(std::isnan(c.decision_mid) && c.queue_ahead == -1, "a fill from before the tape gets no decision-time label");
    const auto [first, last] = study.span();
    check(first == kT0 - 2 * kSec && last == kT0 + 70 * kSec, "the span the tape covered, for telling sessions apart");

    std::printf("%s\n", failures == 0 ? "all live execution study checks passed" : "live execution study checks did not pass");
    return failures == 0 ? 0 : 1;
}
