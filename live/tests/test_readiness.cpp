// The readiness gates on records whose verdicts are known: SIM sessions do
// not count; a clean record passes; a lost frame, a slow session, a recent
// halt, a replay that differed each shut their gate; no evidence is "no
// evidence", never a pass; a model needs days, priced fills, an edge that
// survives the adjustment and the stress, and a bounded drawdown.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/readiness.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair::live;
using namespace altair::live::readiness;

SessionRecord clean(int k) {
    SessionRecord s;
    s.date = "2026-08-" + std::to_string(10 + k % 20);
    s.source = "LIVE";
    s.frames = 10'000'000;
    s.frame_p99_us = 400.0;
    s.decision_p99_us = 3000.0;
    s.tape = "t" + std::to_string(k) + ".tape";
    s.tape_complete = true;
    s.bundle = "b" + std::to_string(k);
    return s;
}
GateStatus find(const std::vector<ReadinessGate>& g, const std::string& name) {
    for (const auto& x : g) if (x.name == name) return x.status;
    return GateStatus::Unknown;
}

} // namespace

int main() {
    std::printf("live readiness\n");
    const ReadinessBars bars;
    std::vector<SessionRecord> s;
    std::set<std::string> bundles;
    for (int k = 0; k < 25; ++k) { s.push_back(clean(k)); bundles.insert(s.back().bundle); }
    std::vector<ReplayVerdict> replays;
    for (int k = 0; k < 6; ++k) replays.push_back(ReplayVerdict{"t", "LIVE", true, true});

    {
        const auto g = operational(s, replays, bundles, bars);
        bool all = true;
        for (const auto& x : g) all = all && x.status == GateStatus::Pass;
        check(all, "a clean record of 25 LIVE sessions passes every operational gate");
    }
    {
        std::vector<SessionRecord> sim;
        for (int k = 0; k < 25; ++k) { auto x = clean(k); x.source = "SIM"; sim.push_back(x); }
        const auto g = operational(sim, {}, bundles, bars);
        check(find(g, "sessions") == GateStatus::Fail && find(g, "feed") == GateStatus::Unknown,
              "SIM sessions prove the plumbing, not the market: they do not count");
        auto replayed = s;
        for (auto& x : replayed) x.replay = true;
        check(find(operational(replayed, replays, bundles, bars), "sessions") == GateStatus::Fail, "nor do replays");
    }
    {
        auto lossy = s;
        lossy[3].missed = 5000;   // 5e-4 of one session, 2e-5 of the whole
        lossy[3].gaps = 1;
        check(find(operational(lossy, replays, bundles, bars), "feed") == GateStatus::Pass,
              "one session with a gap: still a negligible share");
        for (int k = 0; k < 4; ++k) lossy[static_cast<std::size_t>(k)].gaps = 1;
        check(find(operational(lossy, replays, bundles, bars), "feed") == GateStatus::Fail, "four of 25 sessions with a gap: shut");
    }
    {
        auto slow = s;
        slow[7].frame_p99_us = 9000.0;
        check(find(operational(slow, replays, bundles, bars), "latency") == GateStatus::Fail, "one slow session's p99 shuts latency");
        slow[7].frame_p99_us = altair::live::report::kNaN;
        check(find(operational(slow, replays, bundles, bars), "latency") == GateStatus::Unknown, "an unmeasured one is no evidence, not a pass");
    }
    {
        auto halted = s;
        halted[2].halts = 1;   // long ago
        check(find(operational(halted, replays, bundles, bars), "halts") == GateStatus::Pass, "a halt before the last ten sessions is history");
        halted[22].kill_seen = true;
        check(find(operational(halted, replays, bundles, bars), "halts") == GateStatus::Fail, "a kill request in the last ten is not");
        auto broke = s;
        broke[9].exit_code = 4;
        check(find(operational(broke, replays, bundles, bars), "clean exits") == GateStatus::Fail, "a session that left rows unwritten");
    }
    {
        auto r = replays;
        r.push_back(ReplayVerdict{"t", "LIVE", false, true});
        check(find(operational(s, r, bundles, bars), "replays") == GateStatus::Fail, "one replay that differed shuts replays, whatever else passed");
        check(find(operational(s, {}, bundles, bars), "replays") == GateStatus::Unknown, "none at all is no evidence");
        std::set<std::string> fewer = bundles;
        fewer.erase("b4");
        check(find(operational(s, replays, fewer, bars), "bundles") == GateStatus::Fail, "a session whose bundle is missing");
    }

    // ---- per model ----------------------------------------------------------------
    {
        report::DailyStats good;
        good.days = 80; good.mean = 2000; good.ci_lo = 500; good.ci_hi = 3500; good.p_rw = 0.01; good.max_drawdown = 20000;
        report::DailyStats stressed = good;
        stressed.mean = 1200; stressed.ci_lo = 100; stressed.p_rw = 0.03;
        const auto pass = economic("m", good, stressed, 80, 1, false, 500000, bars);
        bool all = true;
        for (const auto& x : pass) all = all && x.status == GateStatus::Pass;
        check(all, "80 days, an edge that survives Romano-Wolf and the stress, a small drawdown: every model gate passes");
        auto young = good;
        young.days = 40;
        check(find(economic("m", young, stressed, 40, 0, false, 500000, bars), "days") == GateStatus::Fail, "40 days are too few");
        check(find(economic("m", good, stressed, 80, 0, true, 500000, bars), "priced") == GateStatus::Fail, "an unpriced fill shuts priced");
        check(find(economic("m", good, stressed, 80, 10, false, 500000, bars), "priced") == GateStatus::Fail, "so do 10 of 80 days unmarked");
        auto lucky = good;
        lucky.p_rw = 0.20;
        check(find(economic("m", lucky, stressed, 80, 0, false, 500000, bars), "edge") == GateStatus::Fail,
              "a raw edge that does not survive the adjustment is not an edge");
        auto thin = stressed;
        thin.ci_lo = -300;
        check(find(economic("m", good, thin, 80, 0, false, 500000, bars), "stress") == GateStatus::Fail, "nor one that the stress erases");
        check(find(economic("m", good, stressed, 80, 0, false, 50000, bars), "drawdown") == GateStatus::Fail,
              "a drawdown of 40 % of the capital used");
        report::DailyStats untested;
        untested.days = 3;
        check(find(economic("m", untested, untested, 3, 0, false, 0, bars), "edge") == GateStatus::Unknown, "too few days to test: no evidence");
    }

    std::printf("%s\n", failures == 0 ? "all live readiness checks passed" : "live readiness checks did not pass");
    return failures == 0 ? 0 : 1;
}
