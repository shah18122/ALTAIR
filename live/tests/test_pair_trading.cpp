// live/tests/test_pair_trading.cpp -- the bank-pairs rule (live/pair_trading.hpp).
#include <live/pair_trading.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace altair::live;

int main() {
    // z of a known series: 19 zeros then 1 -> mean 0.05, sd sqrt(0.05) -> z = 0.95/0.2236 = 4.249
    std::vector<double> s(19, 0.0);
    s.push_back(1.0);
    assert(std::fabs(bank_pair_z(s) - 0.95 / std::sqrt(0.05)) < 1e-9);
    assert(std::isnan(bank_pair_z(std::vector<double>(19, 1.0))));

    // flat, A jumps 5 % against a quiet B on the last close -> A rich: sell A
    std::vector<double> a, b;
    for (int i = 0; i < 25; ++i) { a.push_back(100.0 * (1.0 + 0.001 * (i % 3))); b.push_back(200.0); }
    a.back() = 105.0;
    auto act = bank_pair_decide(a, b, false, 0);
    assert(act.kind == BankPairAction::Enter && act.side_a == -1 && act.z > 2.0);
    // same mirrored -> buy A
    a.back() = 95.0;
    act = bank_pair_decide(a, b, false, 0);
    assert(act.kind == BankPairAction::Enter && act.side_a == 1);
    // too few closes -> nothing
    act = bank_pair_decide(std::vector<double>(a.begin(), a.begin() + 10), std::vector<double>(b.begin(), b.begin() + 10), false, 0);
    assert(act.kind == BankPairAction::None);
    // a 30 % overnight jump inside the window -> skipped as a split
    auto a2 = a;
    for (std::size_t i = a2.size() - 8; i < a2.size(); ++i) a2[i] *= 0.7;
    a2.back() = a2[a2.size() - 2] * 1.06;
    act = bank_pair_decide(a2, b, false, 0);
    assert(act.kind == BankPairAction::None);
    // in the pair: back to the mean -> exit; still stretched -> hold; 11 closes -> exit
    a.back() = a[a.size() - 3];   // i % 3 == 1: the middle value, z ~ 0
    act = bank_pair_decide(a, b, true, 3);
    assert(act.kind == BankPairAction::Exit);
    a.back() = 105.0;
    act = bank_pair_decide(a, b, true, 3);
    assert(act.kind == BankPairAction::None);
    act = bank_pair_decide(a, b, true, 11);
    assert(act.kind == BankPairAction::Exit);
    std::puts("pair trading: ok");
    return 0;
}
