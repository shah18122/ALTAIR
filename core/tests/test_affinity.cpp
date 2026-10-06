// core/affinity.hpp: the plan for a machine's cores, config/latency.toml read
// strictly (a typo is an error naming its line, never a silent unpinned run),
// the shipped file parses, and applying the plan to a thread never throws or
// stops the caller.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <core/affinity.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

} // namespace

int main() {
    using namespace altair::latency;
    std::printf("latency: cores and priorities\n");

    const Plan p8 = default_plan(8);
    check(p8.roles.at("bus").core == 2 && p8.roles.at("feed").core == 3 && p8.roles.at("engine").core == 4
              && p8.roles.at("router").core == 5 && p8.roles.at("tbt").core == 6,
          "8 cores: 0 and 1 left to Windows and the desktop, the roles on 2 to 6, most critical first");
    const Plan p4 = default_plan(4);
    check(p4.roles.at("bus").core == 2 && p4.roles.at("feed").core == 3 && p4.roles.at("engine").core == 2,
          "4 cores: the roles share 2 and 3");
    const Plan p2 = default_plan(2);
    bool none = true;
    for (const auto& [k, r] : p2.roles) none = none && r.core < 0;
    check(none && p2.roles.at("bus").prio == Prio::Highest && p2.roles.at("tbt").prio == Prio::AboveNormal,
          "2 cores: nothing pinned, the priorities still apply");

    std::string err;
    const auto ok = parse_plan("enabled = true\nhigh_priority = false\ntimer_ms = 1\nreserve = 1\n"
                               "[cores]\nbus = 5\nfeed = \"off\"\nengine = \"auto\"\n"
                               "[priority]\nbus = \"time_critical\"   # the owner thread\n",
                               8, err);
    check(ok && !ok->high_priority && ok->roles.at("bus").core == 5 && ok->roles.at("feed").core < 0
              && ok->roles.at("engine").core == 3 && ok->roles.at("bus").prio == Prio::TimeCritical,
          "a number pins, \"off\" unpins, \"auto\" follows the reserve (1 here: engine on core 3)");
    err.clear();
    check(!parse_plan("[cores]\nbsu = 3\n", 8, err) && err.find("line 2") != std::string::npos
              && err.find("bsu") != std::string::npos,
          "a misspelt role is an error naming its line");
    err.clear();
    check(!parse_plan("[cores]\nbus = 12\n", 8, err) && err.find("does not exist") != std::string::npos,
          "a core the machine does not have is refused");
    err.clear();
    check(!parse_plan("[priority]\nbus = \"realtime\"\n", 8, err), "an unknown priority is refused");

    // The shipped file (the test runs from the source tree).
    std::ifstream f("config/latency.toml");
    std::stringstream s;
    s << f.rdbuf();
    err.clear();
    const auto shipped = parse_plan(s.str(), 8, err);
    check(f.good() && shipped && shipped->enabled && shipped->roles.size() == 5, "config/latency.toml parses");
    std::string note;
    const Plan loaded = load_plan("config/latency.toml", note);
    check(loaded.roles.size() == 5 && note.find("latency:") == 0, "and loads, saying what it used");

    // Applying: on whatever this machine allows, it reports and returns.
    std::string said;
    std::thread th([&] { said = apply_thread(default_plan(hardware_cores()), "bus"); });
    th.join();
    std::printf("  (%s)\n", said.c_str());
    check(said.rfind("bus:", 0) == 0, "applying to a thread says what it did and returns");
    Plan off = default_plan(8);
    off.enabled = false;
    check(apply_thread(off, "bus").find("not pinned") != std::string::npos && apply_process(off).find("off") != std::string::npos,
          "switched off, nothing is touched");

    std::printf("%s\n", failures == 0 ? "all latency checks passed" : "latency checks did not pass");
    return failures == 0 ? 0 : 1;
}
