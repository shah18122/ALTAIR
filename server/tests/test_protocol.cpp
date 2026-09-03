// P11-01 acceptance tests.
//
// Test 1 is the card: a nanosecond timestamp does not survive a JavaScript
// number, and a paise amount does. Both halves are measured, because the
// asymmetry is the finding -- a rule of "be careful with big numbers" would
// have guarded the wrong field.
//
// Test 2: coalescing is correct for state and catastrophic for events,
// measured as P&L silently lost.
//
// Test 3: a gap is detectable, and a delta on a stale baseline is refused
// rather than applied.
//
// Test 4: the header round-trips, and a zeroed buffer is not a valid frame.
//
// Test 5: the client vocabulary contains no order, and an event channel
// cannot be configured to drop.
//
// No check description here may contain the substring FAIL.

#include <server/protocol.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

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

double rupees(std::int64_t p) { return static_cast<double>(p) / 100.0; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
};

/// What the client would hold if it decoded a 64-bit field into a JavaScript
/// `number` instead of a BigInt: int64 -> double -> int64.
std::int64_t through_js_number(std::int64_t v)
{
    return static_cast<std::int64_t>(static_cast<double>(v));
}

} // namespace

// ---------------------------------------------------------------------------

using namespace altair;

static void test_js_number_hazard()
{
    std::printf("\n[1] a nanosecond timestamp through a JavaScript number\n");

    // An ordinary trading instant: 2026-09-03, in nanoseconds since the Unix
    // epoch. Nothing exotic -- this is simply what the engine stamps.
    const std::int64_t t0 = 1788393600000000000LL;

    const double res = double_resolution_at(t0);
    std::printf("    engine timestamp  %lld ns\n", static_cast<long long>(t0));
    std::printf("    double spacing at that magnitude  %.0f ns\n", res);
    std::printf("    js_number_safe(timestamp) = %s\n",
                js_number_safe(t0) ? "true" : "false");

    check(!js_number_safe(t0),
          "an engine timestamp in nanoseconds is beyond 2^53, so a JavaScript"
          " number cannot hold it exactly -- this is true of every timestamp"
          " the system will ever emit, not of an unusual one");
    check(res >= 2.0,
          "and the spacing between representable doubles at that magnitude is"
          " larger than one nanosecond, so distinct instants share a value");

    // A burst of 4096 ticks 40 ns apart -- an ordinary option-chain update.
    constexpr std::size_t kN = 4096;
    constexpr std::int64_t kStep = 40;
    std::int64_t exact[kN];
    std::int64_t viajs[kN];
    for (std::size_t i = 0; i < kN; ++i) {
        exact[i] = t0 + static_cast<std::int64_t>(i) * kStep;
        viajs[i] = through_js_number(exact[i]);
    }

    std::size_t distinct = 1;
    std::size_t collapsed = 0;
    std::size_t inverted = 0;
    for (std::size_t i = 1; i < kN; ++i) {
        if (viajs[i] != viajs[i - 1]) { ++distinct; }
        if (viajs[i] == viajs[i - 1]) { ++collapsed; }
        if (viajs[i] < viajs[i - 1]) { ++inverted; }
    }
    std::printf("    burst of %zu ticks %lld ns apart:\n",
                kN, static_cast<long long>(kStep));
    std::printf("      distinct instants, exact          %zu\n", kN);
    std::printf("      distinct instants, via js number  %zu\n", distinct);
    std::printf("      ticks equal to their predecessor  %zu\n", collapsed);
    std::printf("      pairs actually out of order       %zu\n", inverted);

    check(distinct < kN,
          "a burst of ticks 40 ns apart loses most of its distinct instants"
          " when decoded into a JavaScript number");
    check(inverted == 0,
          "and nothing is INVERTED -- rounding to nearest is monotone, so the"
          " ordering is not scrambled, it is ERASED: distinct events become"
          " simultaneous and whatever the client sorts by next decides their"
          " order, which is the failure the replay scrubber and the audit"
          " trail would show");

    // The other half: money is fine, and saying so is the point.
    const std::int64_t big_money = 100LL * 100LL * 100000LL * 10000LL; // Rs 1,000 cr in paise
    std::printf("    Rs 1,000 crore in paise = %lld, js_number_safe = %s\n",
                static_cast<long long>(big_money),
                js_number_safe(big_money) ? "true" : "false");
    check(js_number_safe(big_money)
          && through_js_number(big_money) == big_money,
          "a paise amount of Rs 1,000 crore round-trips through a JavaScript"
          " number exactly -- money is NOT the field at risk here, and the"
          " protocol still carries it as int64 because the ledger reconciles"
          " to the paisa against a broker contract note");

    // Where money WOULD break, for scale.
    const std::int64_t money_limit = kJsMaxSafeInteger;
    std::printf("    a paise amount only loses exactness above Rs %.0f crore\n",
                rupees(money_limit) / 10000000.0);
}

static void test_coalescing()
{
    std::printf("\n[2] coalescing state is right, coalescing events is not\n");

    // 512 fills arriving across 240 frames of a 4-second window at 60 fps.
    constexpr std::size_t kFills = 512;
    constexpr std::size_t kFrames = 240;
    std::int64_t pnl[kFills];
    std::size_t frame_of[kFills];
    Lcg rng{0x5EED1234ULL};
    std::int64_t truth = 0;
    for (std::size_t i = 0; i < kFills; ++i) {
        // Realised P&L per fill, in paise: a few hundred rupees either way.
        pnl[i] = static_cast<std::int64_t>((rng.uniform() - 0.35) * 120000.0);
        frame_of[i] = static_cast<std::size_t>(rng.uniform()
                                               * static_cast<double>(kFrames));
        truth += pnl[i];
    }

    // Coalesce the way the STATE channel does: last writer wins per frame.
    std::int64_t last_in_frame[kFrames] = {};
    bool has[kFrames] = {};
    for (std::size_t i = 0; i < kFills; ++i) {
        last_in_frame[frame_of[i]] = pnl[i];
        has[frame_of[i]] = true;
    }
    std::int64_t coalesced = 0;
    std::size_t survived = 0;
    for (std::size_t f = 0; f < kFrames; ++f) {
        if (has[f]) { coalesced += last_in_frame[f]; ++survived; }
    }

    std::printf("    %zu fills across %zu frames at 60 fps\n", kFills, kFrames);
    std::printf("      realised P&L, every fill counted   Rs %12.2f\n",
                rupees(truth));
    std::printf("      P&L if the event channel coalesces Rs %12.2f  (%zu of"
                " %zu fills survive)\n",
                rupees(coalesced), survived, kFills);
    const double lost = truth != 0
        ? 100.0 * (1.0 - static_cast<double>(coalesced)
                         / static_cast<double>(truth))
        : 0.0;
    std::printf("      silently lost                      %.1f%%\n", lost);

    check(survived < kFills,
          "coalescing drops fills, because two fills in one frame are two"
          " separate facts and not two samples of one state");
    check(std::fabs(static_cast<double>(coalesced)
                    - static_cast<double>(truth)) > 0.0,
          "and the reported P&L is therefore wrong by a number nothing on"
          " screen accounts for -- there is no later frame that restates a"
          " fill, which is exactly what makes it an event rather than a state");

    // AND THE ERROR IS NOT CONSERVATIVE. Above, the fills have a positive
    // mean, so dropping 60% of them understates P&L and the number at least
    // errs toward caution. That is an accident of the sign mix. Re-run with
    // P&L centred on zero -- a market-making tape, or any losing session --
    // and the surviving subsample is just a subsample: it overstates as
    // readily as it understates.
    std::int64_t worst_over = 0;
    std::int64_t worst_under = 0;
    std::size_t overstated = 0;
    for (std::size_t trial = 0; trial < 64; ++trial) {
        Lcg r2{0xA11CE000ULL + trial};
        std::int64_t t2 = 0;
        std::int64_t lif[kFrames] = {};
        bool h2[kFrames] = {};
        for (std::size_t i = 0; i < kFills; ++i) {
            const std::int64_t v =
                static_cast<std::int64_t>((r2.uniform() - 0.5) * 120000.0);
            const std::size_t f = static_cast<std::size_t>(
                r2.uniform() * static_cast<double>(kFrames));
            t2 += v;
            lif[f] = v;
            h2[f] = true;
        }
        std::int64_t c2 = 0;
        for (std::size_t f = 0; f < kFrames; ++f) {
            if (h2[f]) { c2 += lif[f]; }
        }
        const std::int64_t err = c2 - t2;
        if (err > worst_over) { worst_over = err; }
        if (err < worst_under) { worst_under = err; }
        if (err > 0) { ++overstated; }
    }
    std::printf("    with P&L centred on zero, over 64 tapes:\n");
    std::printf("      coalesced total OVERSTATED in       %zu of 64\n",
                overstated);
    std::printf("      worst overstatement                Rs %+12.2f\n",
                rupees(worst_over));
    std::printf("      worst understatement               Rs %+12.2f\n",
                rupees(worst_under));
    check(overstated > 0 && overstated < 64,
          "the coalesced total overstates on some tapes and understates on"
          " others, so this is not a conservative approximation that errs"
          " toward caution -- it is an error of arbitrary sign, and a risk"
          " number that is too small is the direction that costs money");

    check(required_policy(Channel::Event) == Overflow::DisconnectOnBacklog,
          "so the event channel's required policy is to end the session rather"
          " than drop: a dashboard telling you to reconnect is better than one"
          " quietly missing your fills");
    check(required_policy(Channel::State) == Overflow::CoalesceNewest,
          "while the state channel coalesces, which loses nothing -- the next"
          " frame carries the same field's current value");
    check(required_policy(Channel::Unspecified) == Overflow::Unspecified,
          "and an unspecified channel has no policy to fall back on");
}

static void test_gap_detection()
{
    std::printf("\n[3] a gap is detectable, and a stale delta is refused\n");

    SeqTracker t{};
    check(!t.can_apply(),
          "a fresh tracker cannot apply a delta, because no baseline has"
          " arrived yet");

    FrameHeader h{};
    h.channel = Channel::State;
    h.kind = FrameKind::Delta;
    h.seq = 1;
    check(t.accept(h) == SeqVerdict::Gap,
          "a delta arriving before any snapshot is reported as a gap rather"
          " than applied to nothing");

    h.kind = FrameKind::Snapshot;
    h.seq = 100;
    check(t.accept(h) == SeqVerdict::InOrder && t.can_apply(),
          "a snapshot establishes the baseline and clears the block");

    h.kind = FrameKind::Delta;
    h.seq = 101;
    check(t.accept(h) == SeqVerdict::InOrder && t.can_apply(),
          "the next delta applies");

    h.seq = 101;
    check(t.accept(h) == SeqVerdict::Duplicate,
          "a repeat of a frame already seen is a duplicate and is discarded,"
          " which is harmless");

    // Now lose 3 frames.
    const std::uint64_t missed_before = t.frames_missed;
    h.seq = 105;
    const auto v = t.accept(h);
    const std::uint64_t missed_here = t.frames_missed - missed_before;
    std::printf("    frames missed by this gap: %llu (session total %llu,"
                " which includes the orphan delta above)\n",
                static_cast<unsigned long long>(missed_here),
                static_cast<unsigned long long>(t.frames_missed));
    check(v == SeqVerdict::Gap && missed_here == 3,
          "losing frames 102-104 is reported as a gap and the count of missed"
          " frames is carried, not merely a boolean");
    check(!t.can_apply(),
          "and until a fresh snapshot arrives no delta may be applied: a delta"
          " against a baseline the client does not have renders a wrong number"
          " confidently, which is worse than rendering nothing");

    h.kind = FrameKind::Heartbeat;
    const std::uint64_t before = t.last;
    h.seq = 0;
    (void)t.accept(h);
    check(t.last == before,
          "a heartbeat does not advance the sequence, so it can never be"
          " mistaken for a lost delta");

    h.kind = FrameKind::Snapshot;
    h.seq = 106;
    (void)t.accept(h);
    check(t.can_apply(),
          "and a snapshot is what clears the block -- nothing else does");
}

static void test_header_roundtrip()
{
    std::printf("\n[4] the header round-trips, and zero is not a frame\n");

    FrameHeader h{};
    h.kind = FrameKind::Delta;
    h.channel = Channel::Event;
    h.seq = 0xDEADBEEFCAFEULL;
    h.topic = 77;
    h.payload_len = 1234;
    h.engine_time_ns = 1788393600123456789LL;
    h.server_time_ns = 1788393600987654321LL;

    std::uint8_t buf[kFrameHeaderBytes] = {};
    const auto w = encode_header(h, buf, sizeof buf);
    check(w.has_value() && *w == kFrameHeaderBytes,
          "a valid header encodes to exactly 48 bytes");

    const auto r = decode_header(buf, sizeof buf);
    check(r.has_value(),
          "and decodes back");
    check(r && r->seq == h.seq && r->topic == h.topic
            && r->payload_len == h.payload_len
            && r->engine_time_ns == h.engine_time_ns
            && r->server_time_ns == h.server_time_ns
            && r->kind == h.kind && r->channel == h.channel,
          "with every field bit-identical, including both nanosecond"
          " timestamps -- which is only true because they travel as"
          " fixed-width little-endian integers and never as a JSON number");

    std::printf("    engine %lld ns, server %lld ns, difference %lld ns\n",
                static_cast<long long>(r->engine_time_ns),
                static_cast<long long>(r->server_time_ns),
                static_cast<long long>(r->server_time_ns
                                       - r->engine_time_ns));
    check(r->engine_time_ns != r->server_time_ns,
          "engine time and server send time are separate fields, so a replay"
          " -- where they differ by hours -- is visibly a replay instead of a"
          " very calm afternoon");

    std::uint8_t zeros[kFrameHeaderBytes] = {};
    const auto z = decode_header(zeros, sizeof zeros);
    check(!z && z.error() == ProtocolError::BadMagic,
          "a zeroed buffer is refused at the magic, so a zeroed struct can"
          " never pass for a valid frame");

    std::uint8_t bad[kFrameHeaderBytes] = {};
    (void)encode_header(h, bad, sizeof bad);
    bad[4] = static_cast<std::uint8_t>(kWireVersion + 1);
    const auto vm = decode_header(bad, sizeof bad);
    check(!vm && vm.error() == ProtocolError::VersionMismatch,
          "and a frame from a different wire version is refused rather than"
          " reinterpreted against this version's field offsets");

    std::uint8_t nokind[kFrameHeaderBytes] = {};
    (void)encode_header(h, nokind, sizeof nokind);
    nokind[6] = 0;
    const auto nk = decode_header(nokind, sizeof nokind);
    check(!nk && nk.error() == ProtocolError::Unspecified,
          "an ordinal-zero frame kind is refused, because every enum here"
          " reserves zero for Unspecified");

    const auto sb = decode_header(buf, kFrameHeaderBytes - 1);
    check(!sb && sb.error() == ProtocolError::ShortBuffer,
          "and a truncated buffer is a short read rather than a decode of"
          " whatever follows it in memory");
}

static void test_client_vocabulary()
{
    std::printf("\n[5] the client vocabulary, and the policy it cannot pick\n");

    // Every value the enum can hold, enumerated. The point is the census: the
    // client's whole vocabulary is four words and none of them is an order.
    const ClientMsg all[] = {
        ClientMsg::Unspecified, ClientMsg::Subscribe, ClientMsg::Unsubscribe,
        ClientMsg::SnapshotRequest, ClientMsg::KillSwitch
    };
    std::size_t mutating = 0;
    for (const ClientMsg m : all) {
        if (m == ClientMsg::KillSwitch) { ++mutating; }
    }
    std::printf("    client message types: %zu, of which %zu change engine"
                " state\n", sizeof all / sizeof all[0], mutating);
    check(mutating == 1,
          "exactly one client message changes anything in the engine, and it"
          " is the kill switch -- which can only flatten, never open");
    check(static_cast<std::uint8_t>(ClientMsg::Unspecified) == 0,
          "and the zero ordinal is Unspecified, so a zeroed command byte is"
          " not a request to do something");

    check(!policy_ok(Channel::Event, Overflow::CoalesceNewest),
          "an event channel configured to coalesce is rejected -- that pairing"
          " is the mistake test 2 priced");
    check(policy_ok(Channel::Event, Overflow::DisconnectOnBacklog),
          "an event channel that disconnects on backlog is accepted");
    check(policy_ok(Channel::State, Overflow::CoalesceNewest),
          "and a state channel that coalesces is accepted");
    check(!policy_ok(Channel::State, Overflow::Unspecified),
          "while no channel may run with an unspecified policy");
}

int main()
{
    std::printf("P11-01 -- the binary delta-frame wire protocol\n");
    test_js_number_hazard();
    test_coalescing();
    test_gap_detection();
    test_header_roundtrip();
    test_client_vocabulary();
    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}
