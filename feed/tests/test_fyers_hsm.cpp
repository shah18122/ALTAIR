// feed/tests/test_fyers_hsm.cpp -- the FYERS data-socket protocol against
// golden vectors produced by the OFFICIAL FYERS SDK (fyers_hsm_golden.hpp).
// Client frames must match the SDK byte for byte; decoded server frames must
// produce exactly the JSON the SDK hands to on_message.
#include <feed/fyers_adapter.hpp>
#include <feed/fyers_hsm.hpp>
#include "fyers_hsm_golden.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using altair::fyers_hsm::Bytes;

bool same(const Bytes& b, std::string_view golden) {
    return b.size() == golden.size()
        && std::memcmp(b.data(), golden.data(), golden.size()) == 0;
}

const std::uint8_t* bytes_of(std::string_view s) {
    return reinterpret_cast<const std::uint8_t*>(s.data());
}

} // namespace

int main() {
    using namespace altair;
    namespace hsm = altair::fyers_hsm;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };

    // ---- access token -> hsm_key ---------------------------------------
    const auto key = hsm::hsm_key_from_token(fyers_golden::kToken, 1'727'000'000);
    check(key && *key == "a1b2c3d4e5f6hsm", "hsm_key decoded from the APPID:JWT access token");
    check(!hsm::hsm_key_from_token(fyers_golden::kToken, 4'102'444'801)
              && hsm::hsm_key_from_token(fyers_golden::kToken, 4'102'444'801).error()
                     == hsm::HsmError::TokenExpired,
          "expired token is refused before any socket opens");
    check(!hsm::hsm_key_from_token("not-a-jwt", 0), "malformed token is refused");

    // ---- client frames, byte for byte against the SDK ------------------
    const std::vector<std::string> topics{"sf|nse_cm|3045", "if|nse_cm|Nifty 50",
                                          "dp|nse_cm|3045"};
    const std::vector<std::string> one{"sf|nse_cm|3045"};
    check(key && same(hsm::auth_frame(*key, fyers_golden::kSource), fyers_golden::k_auth),
          "auth frame matches the SDK");
    check(same(hsm::mode_frame(false, 11), fyers_golden::k_full_mode), "full-mode frame matches the SDK");
    check(same(hsm::mode_frame(true, 11), fyers_golden::k_lite_mode), "lite-mode frame matches the SDK");
    const auto sub = hsm::topics_frame(true, topics, 11, fyers_golden::kToken.size(),
                                       fyers_golden::kSource.size());
    check(sub && same(*sub, fyers_golden::k_subscribe), "subscribe frame matches the SDK");
    const auto unsub = hsm::topics_frame(false, one, 11, fyers_golden::kToken.size(),
                                         fyers_golden::kSource.size());
    check(unsub && same(*unsub, fyers_golden::k_unsubscribe), "unsubscribe frame matches the SDK");
    check(same(hsm::channel_frame(true, 11), fyers_golden::k_resume), "channel resume frame matches the SDK");
    check(same(hsm::channel_frame(false, 11), fyers_golden::k_pause), "channel pause frame matches the SDK");
    check(same(hsm::ack_frame(123456), fyers_golden::k_ack), "ack frame matches the SDK");
    check(hsm::ping_frame() == Bytes{0, 1, 11}, "ping frame is the SDK's three bytes");
    check(!hsm::topics_frame(true, std::vector<std::string>{}, 11, 10), "empty subscription refused");
    check(!hsm::topics_frame(true, one, 0, 10), "channel 0 refused");

    // ---- symbol-token response -> topics --------------------------------
    const auto parsed = hsm::parse_symbol_tokens(
        R"({"s":"ok","code":200,"message":"","validSymbol":{"NSE:SBIN-EQ":"10100000003045",)"
        R"( "NSE:NIFTY50-INDEX":"101000000026000","NSE:NIFTY24OCTFUT":"101124103135003"},)"
        R"("invalidSymbol":["NSE:NOPE-EQ"]})");
    check(parsed && parsed->valid.size() == 3 && parsed->invalid.size() == 1
              && parsed->valid[0].first == "NSE:SBIN-EQ"
              && parsed->valid[0].second == "10100000003045",
          "symbol-token response parses valid and invalid symbols");
    check(!hsm::parse_symbol_tokens(R"({"s":"error","message":"invalid token"})")
              && !hsm::parse_symbol_tokens(R"({"code":1})"),
          "symbol-token rejection and garbage are refused");
    using DT = hsm::DataType;
    check(hsm::topic_for("NSE:SBIN-EQ", "10100000003045", DT::SymbolUpdate) == "sf|nse_cm|3045"
              && hsm::topic_for("NSE:SBIN-EQ", "10100000003045", DT::DepthUpdate) == "dp|nse_cm|3045"
              && hsm::topic_for("NSE:NIFTY24OCTFUT", "101124103135003", DT::SymbolUpdate)
                     == "sf|nse_fo|35003",
          "equity and F&O topics follow the SDK's segment and token rule");
    check(hsm::topic_for("NSE:NIFTY50-INDEX", "101000000026000", DT::SymbolUpdate)
                  == "if|nse_cm|Nifty 50"
              && hsm::topic_for("BSE:SENSEX-INDEX", "121000000001", DT::SymbolUpdate)
                     == "if|bse_cm|SENSEX"
              && hsm::topic_for("NSE:NEWIDX-INDEX", "101000000099999", DT::SymbolUpdate)
                     == "if|nse_cm|NEWIDX",
          "index topics use the SDK's exchange-name table, then its fallback");
    check(hsm::topic_for("NSE:NIFTY50-INDEX", "101000000026000", DT::DepthUpdate).empty()
              && hsm::topic_for("XXX:FOO-EQ", "99990000001", DT::SymbolUpdate).empty(),
          "index depth and unknown segments are skipped, as the SDK does");

    // ---- server frames -> the SDK's on_message JSON ---------------------
    const auto map_all = [](hsm::HsmSession& s) {
        s.map_topic("sf|nse_cm|3045", "NSE:SBIN-EQ");
        s.map_topic("if|nse_cm|Nifty 50", "NSE:NIFTY50-INDEX");
        s.map_topic("dp|nse_cm|3045", "NSE:SBIN-EQ");
    };
    hsm::HsmSession full{false}, lite{true};
    map_all(full);
    map_all(lite);
    std::vector<std::string> snapshot_json;
    for (const auto& c : fyers_golden::kCases) {
        hsm::HsmSession& s = c.lite ? lite : full;
        std::vector<std::string> got;
        const auto r = s.on_frame(bytes_of(c.frame), c.frame.size(),
                                  [&got, &s](const hsm::HsmUpdate& u) {
                                      std::string j;
                                      hsm::render_sdk_json(u, s.lite(), j);
                                      got.push_back(std::move(j));
                                  });
        if (r.event != hsm::HsmEvent::Data)
            got.emplace_back(std::string{"EVENT:"} + hsm::event_text(r.event));
        bool ok = static_cast<int>(got.size()) == c.count;
        for (int i = 0; ok && i < c.count; ++i) ok = got[static_cast<std::size_t>(i)] == c.out[i];
        if (!ok) {
            for (const auto& g : got) std::printf("   got      %s\n", g.c_str());
            for (int i = 0; i < c.count; ++i) std::printf("   expected %s\n", c.out[i]);
        }
        const std::string label = std::string{"decoded frame '"} + std::string{c.name}
                                + "' equals the SDK's on_message output";
        check(ok, label.c_str());
        if (c.name == "upd")
            check(r.ack && r.ack_bytes == hsm::ack_frame(1002),
                  "ack is due after every ack_every data frames, for the last message number");
        if (c.name == "snap") snapshot_json = got;
    }

    // ---- deliberate deviation: the absent-value sentinel never lands ----
    {
        hsm::HsmSession s{false};
        map_all(s);
        std::vector<std::string> got;
        const auto emit = [&got, &s](const hsm::HsmUpdate& u) {
            std::string j;
            hsm::render_sdk_json(u, s.lite(), j);
            got.push_back(std::move(j));
        };
        for (const auto& c : fyers_golden::kCases)
            if (c.name == "auth_ok" || c.name == "snap")
                (void)s.on_frame(bytes_of(c.frame), c.frame.size(), emit);
        got.clear();
        // index topic id 8: one field, ltp = INT32_MIN (absent)
        const std::uint8_t frame[] = {0, 16, 6, 0, 0, 0x03, 0xEA, 0, 1,
                                      85, 8, 0, 1, 0x80, 0, 0, 0};
        const auto r = s.on_frame(frame, sizeof frame, emit);
        check(r.event == hsm::HsmEvent::Data && got.empty(),
              "an absent index field is ignored (the SDK stores -2147483648 there)");
    }

    // ---- truncation never reads past the frame --------------------------
    {
        const auto& snap = fyers_golden::kCases[2];
        bool bounded = true;
        for (std::size_t n = 0; n < snap.frame.size(); ++n) {
            hsm::HsmSession s{false};
            map_all(s);
            std::vector<std::uint8_t> copy(bytes_of(snap.frame), bytes_of(snap.frame) + n);
            const auto r = s.on_frame(copy.data(), copy.size(), [](const hsm::HsmUpdate&) {});
            if (r.event != hsm::HsmEvent::Malformed) bounded = false;
        }
        check(bounded, "every truncated data frame is reported malformed, never over-read");
    }

    // ---- end to end: socket JSON -> existing FYERS adapter -> Tick -------
    {
        static SpecStore specs;
        ContractSpec spec{};
        std::memcpy(spec.symbol, "NSE:SBIN-EQ", 12);
        std::memcpy(spec.underlying, "SBIN", 5);
        spec.lot_size = LotSize{1};
        spec.tick_size = Price{5};
        spec.price_scale = 100;
        spec.valid_from = Timestamp{1};
        spec.valid_to = Timestamp::max();
        spec.snapshot_at = Timestamp{1};
        spec.source_hash = 1;
        const auto id = specs.add(spec);
        FyersDataAdapter<4, 8, 4> adapter{specs, 11};
        (void)adapter.subscribe("NSE:SBIN-EQ", FyersDataMode::SymbolUpdate);
        const auto epoch = adapter.connected();
        adapter.subscriptions_sent();
        bool accepted = !snapshot_json.empty()
            && adapter.on_message(snapshot_json[0], Timestamp{1'000}, epoch);
        FeedEnvelope<Tick> tick{};
        const bool popped = adapter.try_pop(tick);
        check(id.has_value() && accepted && popped && tick.value.last.raw() == 61235
                  && tick.kind == FyersEventKind::Trade,
              "socket JSON flows through FyersDataAdapter into a Tick at 612.35 rupees");
    }

    // ---- one decoder, two inputs: the typed path equals the JSON path ------
    {
        static SpecStore specs;
        ContractSpec spec{};
        std::memcpy(spec.symbol, "NSE:SBIN-EQ", 12);
        std::memcpy(spec.underlying, "SBIN", 5);
        spec.lot_size = LotSize{1};
        spec.tick_size = Price{5};
        spec.price_scale = 100;
        spec.valid_from = Timestamp{1};
        spec.valid_to = Timestamp::max();
        spec.snapshot_at = Timestamp{1};
        spec.source_hash = 1;
        const auto id = specs.add(spec);
        FyersDataAdapter<4, 16, 16> via_json{specs, 11};
        FyersDataAdapter<4, 16, 16> via_fields{specs, 11};
        const auto e1 = via_json.connected();
        const auto e2 = via_fields.connected();
        hsm::HsmSession s{false};
        s.map_topic("sf|nse_cm|3045", "NSE:SBIN-EQ", static_cast<std::uint32_t>(*id));
        s.map_topic("dp|nse_cm|3045", "NSE:SBIN-EQ", static_cast<std::uint32_t>(*id));
        const auto emit = [&](const hsm::HsmUpdate& u) {
            std::string j;
            hsm::render_sdk_json(u, false, j);
            (void)via_json.on_message(j, Timestamp{5}, e1);
            FyersFields f{};
            if (hsm::to_fyers_fields(u, static_cast<InstrumentId>(u.cookie), f))
                (void)via_fields.on_fields(f, Timestamp{5}, e2);
        };
        for (const auto& c : fyers_golden::kCases)
            if (c.name == "auth_ok" || c.name == "snap" || c.name == "upd")
                (void)s.on_frame(bytes_of(c.frame), c.frame.size(), emit);
        bool same = true;
        int ticks = 0, depths = 0;
        FeedEnvelope<Tick> a{}, b{};
        while (via_json.try_pop(a)) {
            same = same && via_fields.try_pop(b) && a.kind == b.kind
                && a.value.id == b.value.id && a.value.last.raw() == b.value.last.raw()
                && a.value.volume.raw() == b.value.volume.raw()
                && a.value.last_qty.raw() == b.value.last_qty.raw()
                && a.value.exchange_ts == b.value.exchange_ts
                && a.value.flags == b.value.flags;
            ++ticks;
        }
        FeedEnvelope<DepthUpdate> da{}, db{};
        while (via_json.try_pop(da)) {
            same = same && via_fields.try_pop(db) && da.value.bid_levels == db.value.bid_levels
                && da.value.bid[0].px.raw() == db.value.bid[0].px.raw()
                && da.value.ask[4].px.raw() == db.value.ask[4].px.raw()
                && da.value.bid[2].qty.raw() == db.value.bid[2].qty.raw()
                && da.value.ask[1].orders == db.value.ask[1].orders;
            ++depths;
        }
        check(same && ticks == 2 && depths == 1 && !via_fields.try_pop(b),
              "the typed socket path yields the same Ticks and depth as the JSON path");
        check(hsm::hsm_paise(61235, 2, 1, b.value.last) && b.value.last.raw() == 61235
                  && !hsm::hsm_paise(123457, 4, 1, b.value.last),
              "wire integers convert to paise exactly, and sub-paisa prices are refused");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
