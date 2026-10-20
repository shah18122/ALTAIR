// Tests for app/iv_record.hpp -- each minute's ATM IV from the streamed chain.

#include <app/iv_record.hpp>

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace ir = altair::iv_record;
namespace lv = altair::live;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

double b76(bool call, double F, double K, double T, double vol) {
    const auto g = altair::black76(call ? altair::OptionRight::Call : altair::OptionRight::Put, altair::Price{std::llround(F * 100.0)},
                                   altair::Price{std::llround(K * 100.0)}, altair::Years{T}, altair::Vol{vol}, 0.065);
    return g ? g->price / 100.0 : 0.0;
}

} // namespace

int main() {
    std::printf("ATM IV from the streamed chain\n");
    // A weekly expiry 6 days out; strikes 24,800 .. 25,200; the forward 25,030.
    const std::int64_t today = 20'500, expiry = today + 6;
    std::vector<lv::LiveInstrument> uni;
    std::uint32_t tok = 1000;
    std::vector<std::pair<std::uint32_t, std::pair<double, bool>>> legs;
    for (double K = 24'800; K <= 25'200; K += 100) {
        for (const bool call : {true, false}) {
            lv::LiveInstrument in;
            in.token = ++tok;
            in.underlying = "NIFTY";
            in.kind = call ? lv::LiveKind::Call : lv::LiveKind::Put;
            in.expiry_day = expiry;
            in.strike = K;
            uni.push_back(in);
            legs.push_back({tok, {K, call}});
        }
    }
    ir::AtmIvRecorder r(uni, "NIFTY");
    // 10:00:00 IST to 10:02:30 IST, prices at 13 % vol every 10 s.
    const std::int64_t ist0 = today * 86'400 + 10 * 3600;
    const double F = 25'030.0;
    for (std::int64_t s = 0; s < 150; s += 10) {
        const std::int64_t ist = ist0 + s;
        const double T = static_cast<double>(expiry * 86'400 + ir::kExpirySec - (ist / 60 + 1) * 60) / (365.0 * 86'400.0);
        for (const auto& [t, kc] : legs) {
            r.on_price(t, b76(kc.second, F, kc.first, T, 0.13), (ist - ir::kIstOffsetSec) * 1'000'000'000LL);
        }
    }
    r.finish();
    const auto& m = r.minutes();
    check(m.size() == 3, "one reading per minute streamed");
    bool ok = !m.empty();
    for (const auto& x : m) {
        ok = ok && std::fabs(x.iv - 0.13) < 1e-3 && std::fabs(x.strike - 25'000.0) < 1e-9 && std::fabs(x.forward - F) < 0.5
          && x.t % 60 == 0 && x.expiry_day == expiry;
    }
    check(ok, "the vol the chain was priced at, at the strike nearest the parity forward, stamped at the minute's end");

    // Stale legs do not price a minute: nothing for 90 s, then one leg only.
    ir::AtmIvRecorder s(uni, "NIFTY");
    s.on_price(legs[0].first, 300.0, (ist0 - ir::kIstOffsetSec) * 1'000'000'000LL);
    s.on_price(legs[2].first, 250.0, (ist0 + 90 - ir::kIstOffsetSec) * 1'000'000'000LL);
    s.finish();
    check(s.minutes().empty(), "a minute without a fresh call and put at one strike gives no reading");

    // The last day: no reading.
    std::vector<lv::LiveInstrument> last = uni;
    for (auto& in : last) in.expiry_day = today;
    ir::AtmIvRecorder e(last, "NIFTY");
    for (const auto& [t, kc] : legs) e.on_price(t, kc.second ? 40.0 : 30.0, (ist0 - ir::kIstOffsetSec) * 1'000'000'000LL);
    e.finish();
    check(e.minutes().empty(), "under a day to expiry gives no reading");

    std::printf("ATM IV from the streamed chain: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
