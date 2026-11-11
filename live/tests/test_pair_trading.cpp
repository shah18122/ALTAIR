// live/tests/test_pair_trading.cpp -- the bank-pairs rule (live/pair_trading.hpp).
#include <live/pair_trading.hpp>
#include <live/sim.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace altair::live;

namespace {

// Checked in every build type (assert would vanish under NDEBUG in Release).
void need(bool ok) {
    if (!ok) { std::puts("pair trading: a check did not hold"); std::exit(1); }
}

// A pair held into the futures' expiry day: the engine's 15:15 roll closes the legs, and the model
// must reopen them in the next month with the same sides, keeping its entry day.
void expiry_roll() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "altair_pair_trading_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::int64_t today = parse_day("2026-11-24");   // the November futures expire today
    const std::int64_t nx = parse_day("2026-12-29");
    std::vector<LiveInstrument> u;
    const auto add = [&u](std::uint32_t tok, const std::string& sym, const std::string& under, LiveKind k, std::int64_t lot,
                          std::int64_t expiry) {
        LiveInstrument i;
        i.token = tok; i.symbol = sym; i.fyers = "NSE:" + sym; i.underlying = under; i.kind = k;
        i.lot = lot; i.expiry_day = expiry; i.tick = 0.05;
        u.push_back(i);
    };
    add(kLiveNiftyToken, "NIFTY 50", "NIFTY", LiveKind::Index, 1, 0);
    add(101, "ICICIBANK", "ICICIBANK", LiveKind::Equity, 1, 0);
    add(102, "KOTAKBANK", "KOTAKBANK", LiveKind::Equity, 1, 0);
    add(111, "ICICIBANK26NOVFUT", "ICICIBANK", LiveKind::Future, 700, today);
    add(112, "ICICIBANK26DECFUT", "ICICIBANK", LiveKind::Future, 700, nx);
    add(121, "KOTAKBANK26NOVFUT", "KOTAKBANK", LiveKind::Future, 400, today);
    add(122, "KOTAKBANK26DECFUT", "KOTAKBANK", LiveKind::Future, 400, nx);

    // 25 closes up to yesterday, the ratio drifting up: z ~ +1.6, so the pair keeps holding
    std::map<std::string, BankPairsCloses::Series> warm;
    for (int k = 25; k >= 1; --k) {
        warm["ICICIBANK"][today - k] = 1300.0 * (1.0 + 0.001 * (25 - k));
        warm["KOTAKBANK"][today - k] = 2000.0;
    }
    { std::ofstream o(dir / "state_ICICIBANK_KOTAKBANK.csv"); o << (today - 3) << ",1\n"; }   // bought A 3 closes ago

    LiveEngine e(u, [](const LiveInstrument&, bool, double, double, std::int64_t) { return 0.0; });
    auto hub = std::make_unique<BankPairsCloses>(warm, "");
    const BankPairsCloses* closes = hub.get();
    e.add_model(std::move(hub));
    e.add_model(std::make_unique<LiveBankPairModel>("ICICIBANK", "KOTAKBANK", closes, dir.string()));
    const std::string model = "Pair trading ICICIBANK/KOTAKBANK";

    LiveSimSeeds seeds;
    seeds.stocks = {{"ICICIBANK", 1330.0}, {"KOTAKBANK", 2000.0}};
    const std::int64_t open_ns = (today * 86400 + 9 * 3600 + 15 * 60 - 19800) * 1'000'000'000LL;
    LiveSim sim(u, seeds, 11, open_ns);
    sim.board([&](const LiveInstrument&, const LiveSimEvent& ev) { e.on_quote(ev.quote, open_ns); });
    bool opened = false, checked_hold = false, checked_roll = false;
    for (int step = 0; step < 6 * 3600 + 15 * 60; ++step) {
        sim.step(1'000'000'000, [&](const LiveInstrument&, const LiveSimEvent& ev) {
            e.on_quote(ev.quote, sim.now_ns());
            if (ev.trade) e.on_trade(ev.price, ev.price.exchange_ts_ns);
        });
        e.advance(sim.now_ns());
        const int mod = live_minute_of_day(live_ist_minute_index(e.clock_ns()));
        if (!opened && mod >= 9 * 60 + 16) {   // the legs carried in from earlier days, November futures
            opened = e.book().open(model, u[3], 1, 1, e.clock_ns(), "carried", true)
                  && e.book().open(model, u[5], -1, 2, e.clock_ns(), "carried", true);
            need(opened);
        }
        if (!checked_hold && mod >= 9 * 60 + 25) {
            checked_hold = true;
            need(e.book().position(model, 111) != nullptr && e.book().position(model, 121) != nullptr);
        }
        if (!checked_roll && mod >= 15 * 60 + 20) {
            checked_roll = true;
            need(e.book().position(model, 111) == nullptr && e.book().position(model, 121) == nullptr);
            const LivePosition* pa = e.book().position(model, 112);
            const LivePosition* pb = e.book().position(model, 122);
            need(pa != nullptr && pb != nullptr && pa->side == 1 && pb->side == -1 && pa->carry && pb->carry);
        }
    }
    need(opened && checked_hold && checked_roll);
    std::ifstream st(dir / "state_ICICIBANK_KOTAKBANK.csv");
    std::stringstream ss;
    ss << st.rdbuf();
    need(ss.str() == std::to_string(today - 3) + ",1\n");   // entry day kept: the 10-session clock runs on
    std::ifstream dec(dir / "decisions.csv");
    std::stringstream ds;
    ds << dec.rdbuf();
    need(ds.str().find(",ROLL,") != std::string::npos && ds.str().find("holding") != std::string::npos);
    fs::remove_all(dir);
}

}  // namespace

int main() {
    // z of a known series: 19 zeros then 1 -> mean 0.05, sd sqrt(0.05) -> z = 0.95/0.2236 = 4.249
    std::vector<double> s(19, 0.0);
    s.push_back(1.0);
    need(std::fabs(bank_pair_z(s) - 0.95 / std::sqrt(0.05)) < 1e-9);
    need(std::isnan(bank_pair_z(std::vector<double>(19, 1.0))));

    // flat, A jumps 5 % against a quiet B on the last close -> A rich: sell A
    std::vector<double> a, b;
    for (int i = 0; i < 25; ++i) { a.push_back(100.0 * (1.0 + 0.001 * (i % 3))); b.push_back(200.0); }
    a.back() = 105.0;
    auto act = bank_pair_decide(a, b, false, 0);
    need(act.kind == BankPairAction::Enter && act.side_a == -1 && act.z > 2.0);
    // same mirrored -> buy A
    a.back() = 95.0;
    act = bank_pair_decide(a, b, false, 0);
    need(act.kind == BankPairAction::Enter && act.side_a == 1);
    // too few closes -> nothing
    act = bank_pair_decide(std::vector<double>(a.begin(), a.begin() + 10), std::vector<double>(b.begin(), b.begin() + 10), false, 0);
    need(act.kind == BankPairAction::None);
    // a 30 % overnight jump inside the window -> skipped as a split
    auto a2 = a;
    for (std::size_t i = a2.size() - 8; i < a2.size(); ++i) a2[i] *= 0.7;
    a2.back() = a2[a2.size() - 2] * 1.06;
    act = bank_pair_decide(a2, b, false, 0);
    need(act.kind == BankPairAction::None);
    // in the pair: back to the mean -> exit; still stretched -> hold; 11 closes -> exit
    a.back() = a[a.size() - 3];   // i % 3 == 1: the middle value, z ~ 0
    act = bank_pair_decide(a, b, true, 3);
    need(act.kind == BankPairAction::Exit);
    a.back() = 105.0;
    act = bank_pair_decide(a, b, true, 3);
    need(act.kind == BankPairAction::None);
    act = bank_pair_decide(a, b, true, 11);
    need(act.kind == BankPairAction::Exit);
    expiry_roll();
    std::puts("pair trading: ok");
    return 0;
}
