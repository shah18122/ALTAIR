// The FYERS 50-level book decoder against messages the official SDK encoded
// (feed/tests/vectors/fyers_tbt.txt): a full snapshot gives fifty levels a
// side; an update changes only the fields whose wrappers are present, an empty
// wrapper meaning zero; an error carries the server's reason; truncated or
// malformed bytes are refused whole. Plus what the client sends.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <feed/fyers_tbt.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

std::vector<std::vector<std::uint8_t>> vectors() {
    std::vector<std::vector<std::uint8_t>> out;
    std::ifstream in(ALTAIR_TBT_VECTORS);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::uint8_t> b;
        for (std::size_t i = 0; i + 1 < line.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoul(line.substr(i, 2), nullptr, 16)));
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace

int main() {
    using namespace altair::fyers_tbt;
    std::printf("FYERS 50-level book\n");
    const auto v = vectors();
    check(v.size() == 3, "three vectors: a snapshot, an update, an error");
    if (v.size() != 3) return 1;

    TbtBooks books;
    TbtMessage m;
    check(books.on_message(v[0].data(), v[0].size(), m) && !m.error && m.updated.size() == 1
              && m.updated[0] == "NSE:NIFTY26OCTFUT",
          "the snapshot decodes, for its ticker");
    const TbtBook* b = books.book("NSE:NIFTY26OCTFUT");
    check(b != nullptr && b->depth() == 50, "fifty levels a side");
    if (b == nullptr) return 1;
    check(b->bid_px[0] == 2500000 && b->bid_px[49] == 2499510 && b->ask_px[0] == 2500010 && b->ask_px[49] == 2500500,
          "prices in paise, best first, to the fiftieth level");
    check(b->bid_qty[0] == 75 && b->bid_qty[49] == 3750 && b->bid_orders[49] == 50 && b->ask_qty[10] == 150
              && b->ask_orders[10] == 2,
          "quantities and order counts by level");
    check(b->total_bid_qty == 123456 && b->total_ask_qty == 654321 && b->sequence == 42 && b->feed_time == 1791200000123ULL,
          "totals, sequence and feed time");

    check(books.on_message(v[1].data(), v[1].size(), m) && m.updated.size() == 1, "the update decodes");
    check(b->bid_qty[0] == 300 && b->bid_px[0] == 2500000, "a changed quantity changes; its price, absent, stays");
    check(b->ask_px[2] == 2500025 && b->ask_qty[2] == 150, "a changed price at level 3 changes; its quantity stays");
    check(b->bid_qty[3] == 0 && b->bid_px[3] == 2499970, "an empty wrapper is a change to zero");
    check(b->bid_qty[1] == 150 && b->ask_px[49] == 2500500 && b->sequence == 43, "every other level is untouched");

    check(books.on_message(v[2].data(), v[2].size(), m) && m.error && m.text == "invalid symbol NSE:XYZ" && m.updated.empty(),
          "an error carries the server's reason and changes no book");

    std::vector<std::uint8_t> cut(v[0].begin(), v[0].begin() + static_cast<std::ptrdiff_t>(v[0].size() / 2));
    TbtBooks fresh;
    check(!fresh.on_message(cut.data(), cut.size(), m) && fresh.book("NSE:NIFTY26OCTFUT") == nullptr,
          "a message cut in half is refused whole");
    const std::uint8_t junk[] = {0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    check(!fresh.on_message(junk, sizeof junk, m), "a length past the end is refused");

    // ---- what the client sends --------------------------------------------------------
    check(tbt_subscribe_text({"NSE:NIFTY26OCTFUT", "NSE:SBIN-EQ"}, 3)
              == R"({"type":1,"data":{"subs":1,"symbols":["NSE:NIFTY26OCTFUT","NSE:SBIN-EQ"],"mode":"depth","channel":"3"}})",
          "a subscription names the symbols, depth mode and the channel");
    check(tbt_resume_text(2) == R"({"type":2,"data":{"resumeChannels":["1","2"],"pauseChannels":[]}})",
          "and the channels are resumed");
    std::vector<std::string> many;
    for (int i = 0; i < 260; ++i) many.push_back("S" + std::to_string(i));
    std::vector<std::string> left;
    const auto ch = tbt_channels(many, &left);
    check(ch.size() == 50 && ch[0].size() == 5 && ch[49].back() == "S249" && left.size() == 10 && left[0] == "S250",
          "symbols go five to a channel, fifty channels; the rest are reported, not dropped");
    const auto url = tbt_socket_url(R"({"s":"ok","data":{"socket_url":"wss://rtsocket-api.fyers.in/versova"}})");
    check(url && url->first == "rtsocket-api.fyers.in" && url->second == "/versova", "the socket URL splits into host and path");
    check(!tbt_socket_url(R"({"data":{"socket_url":"http://x/y"}})"), "a URL that is not wss is not used");

    std::printf("%s\n", failures == 0 ? "all FYERS 50-level checks passed" : "FYERS 50-level checks did not pass");
    return failures == 0 ? 0 : 1;
}
