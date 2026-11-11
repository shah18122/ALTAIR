// research/equity/bank_pairs_replay.cpp -- run the live rule (live/bank_pairs.hpp, bank_pair_decide)
// over a closes file and print its trades, to check it against research/equity/pairs.py.
//
//   bank_pairs_replay CLOSES.csv A B      (CLOSES.csv: day,SYM,close; one row per stock per session)
// Prints entry_day,exit_day,side_a (the morning each was decided).
#include <live/bank_pairs.hpp>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s CLOSES.csv A B\n", argv[0]); return 2; }
    std::map<std::string, std::map<std::string, double>> c;
    std::ifstream in(argv[1]);
    std::string line;
    while (std::getline(in, line)) {
        const auto p = line.find(','), q = line.find(',', p + 1);
        if (p == std::string::npos || q == std::string::npos) continue;
        c[line.substr(p + 1, q - p - 1)][line.substr(0, p)] = std::atof(line.c_str() + q + 1);
    }
    std::vector<std::string> days;
    std::vector<double> ca, cb;
    for (const auto& [d, pa] : c[argv[2]]) {
        const auto it = c[argv[3]].find(d);
        if (it == c[argv[3]].end() || !(pa > 0.0) || !(it->second > 0.0)) continue;
        days.push_back(d); ca.push_back(pa); cb.push_back(it->second);
    }
    bool held = false;
    std::size_t entry = 0;
    int side = 0;
    std::puts("entry,exit,side_a");
    for (std::size_t d = 1; d < days.size(); ++d) {   // the morning of day d, closes up to d-1
        const std::vector<double> a(ca.begin(), ca.begin() + static_cast<long>(d)), b(cb.begin(), cb.begin() + static_cast<long>(d));
        const int since = held ? static_cast<int>(d - entry) : 0;
        const auto act = altair::live::bank_pair_decide(a, b, held, since);
        if (held && act.kind == altair::live::BankPairAction::Exit) {
            std::printf("%s,%s,%d\n", days[entry].c_str(), days[d].c_str(), side);
            held = false;
        } else if (!held && act.kind == altair::live::BankPairAction::Enter) {
            held = true; entry = d; side = act.side_a;
        }
    }
    return 0;
}
