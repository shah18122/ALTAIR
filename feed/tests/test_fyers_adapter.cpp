#include <feed/fyers_adapter.hpp>
#include <feed/source_router.hpp>

#include <cstdio>
#include <cstring>

int main() {
    using namespace altair;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    static SpecStore specs;
    ContractSpec spec{};
    spec.token[static_cast<std::size_t>(FeedSource::Fyers)] = 123;
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
    check(id.has_value(), "fixture has canonical FYERS identity");

    FyersDataAdapter<2, 4, 2> adapter{specs, 7};
    check(adapter.subscribe("NSE:SBIN-EQ", FyersDataMode::SymbolUpdate)
              && adapter.subscribe("NSE:SBIN-EQ", FyersDataMode::DepthUpdate)
              && adapter.subscription_count() == 2 && adapter.channel() == 7,
          "subscription book is bounded, typed and channel-scoped");
    check(!adapter.subscribe("NSE:INFY-EQ", FyersDataMode::SymbolUpdate),
          "subscription capacity refuses overflow");
    const auto epoch = adapter.connected();
    check(epoch == 1 && adapter.subscriptions_dirty(),
          "connect starts an epoch and requires subscription replay");
    adapter.subscriptions_sent();

    constexpr auto trade =
        R"({"type":"sf","seq":10,"fyToken":"123","ltp":250.05,"last_traded_qty":7,"last_traded_time":1700000000})";
    constexpr auto quote =
        R"({"type":"sf","seq":11,"fyToken":"123","ltp":250.10,"last_traded_time":1700000001})";
    check(adapter.on_message(trade, Timestamp{1'700'000'000'100'000'000LL}, epoch)
              && adapter.on_message(quote, Timestamp{1'700'000'001'100'000'000LL}, epoch),
          "SDK JSON callback distinguishes and queues trade and quote");
    check(!adapter.on_message(quote, Timestamp{1'700'000'001'100'000'000LL}, epoch)
              && adapter.stats().duplicate_sequence == 1,
          "duplicate provider sequence is refused");
    adapter.disconnected(epoch);
    check(!adapter.on_message(trade, Timestamp{1}, epoch)
              && adapter.stats().stale_epoch == 1,
          "late callback from disconnected socket is refused");
    const auto reconnect = adapter.connected();
    check(reconnect == 2 && adapter.stats().reconnects == 1
              && adapter.subscriptions_dirty(),
          "reconnect creates a new epoch and resubscribes");
    check(adapter.on_message(
              R"({"type":"dp","seq":20,"fyToken":123,"bid_price1":250.00,"ask_price1":250.05,"bid_size1":10,"ask_size1":12})",
              Timestamp{1'700'000'002'100'000'000LL}, reconnect),
          "depth update is decoded in the reconnect epoch");
    check(adapter.on_message(
              R"({"type":"sf","seq":22,"fyToken":123,"ltp":250.15})",
              Timestamp{1'700'000'003'100'000'000LL}, reconnect)
              && adapter.stats().sequence_gaps == 1,
          "provider sequence gap is observable without blocking ingestion");
    FeedEnvelope<Tick> first{};
    FeedEnvelope<Tick> second{};
    FeedEnvelope<DepthUpdate> depth{};
    check(adapter.try_pop(first) && first.kind == FyersEventKind::Trade
              && adapter.try_pop(second) && second.kind == FyersEventKind::Quote
              && adapter.try_pop(depth) && depth.kind == FyersEventKind::Depth,
          "trade, quote and depth remain distinct at the consumer boundary");

    Normaliser::Config cfg{};
    cfg.active = FeedSource::Fyers;
    static SourceRouter<2, 2> router{cfg};
    check(router.activate(FeedSource::Fyers, reconnect),
          "headless router selects verified FYERS epoch");
    const Timestamp local{1'700'000'010'000'000'000LL};
    check(router.submit(depth, local), "FYERS depth enters shared normaliser");
    check(!router.submit(depth, local) && router.stats().duplicates == 1,
          "duplicate depth never reaches book consumers");
    FeedEnvelope<Tick> fresh{second.value, reconnect, second.kind};
    check(router.submit(fresh, local), "FYERS tick enters shared normaliser");
    FeedEnvelope<Tick> old{second.value, epoch, second.kind};
    check(!router.submit(old, local) && router.stats().stale_epoch == 1,
          "old socket epoch cannot enter the pipeline");
    const auto old_depth_generation = router.depth_generation();
    check(router.activate(FeedSource::Kite, 9)
              && router.depth_generation() == old_depth_generation + 1,
          "source transition forces consumers to reset incompatible depth");
    check(!router.submit(depth, local),
          "stale cross-source depth is never combined after failover");
    Tick published{};
    DepthUpdate published_depth{};
    check(router.try_pop(published) && published.source == FeedSource::Fyers
              && router.try_pop(published_depth)
              && published_depth.source == FeedSource::Fyers,
          "headless output works without any QWidget/grid/chart construction");
    check(adapter.unsubscribe("NSE:SBIN-EQ", FyersDataMode::DepthUpdate)
              && adapter.subscription_count() == 1,
          "unsubscribe updates the bounded replay set");

    FyersDataAdapter<1, 2, 2> bounded{specs};
    const auto bounded_epoch = bounded.connected();
    check(bounded.on_message(
              R"({"type":"sf","seq":1,"fyToken":123,"ltp":250.00})",
              Timestamp{100}, bounded_epoch)
              && bounded.on_message(
                  R"({"type":"sf","seq":2,"fyToken":123,"ltp":250.05})",
                  Timestamp{101}, bounded_epoch)
              && !bounded.on_message(
                  R"({"type":"sf","seq":3,"fyToken":123,"ltp":250.10})",
                  Timestamp{102}, bounded_epoch)
              && bounded.stats().backpressure_drops == 1,
          "full callback ring refuses and counts backpressure without overwrite");
    return failures == 0 ? 0 : 1;
}
