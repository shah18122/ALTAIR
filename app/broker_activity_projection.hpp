// app/broker_activity_projection.hpp -- bounded service -> desktop projection.
//
// This writer is deliberately separate from the desktop. It receives immutable
// read-side service accessors, emits only the bounded redacted ring, and replaces
// the destination atomically. The desktop never needs credentials or a broker
// library to render the result.
#pragma once

#include <types/broker_state.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace altair::app {

enum class ActivityProjectionError : std::uint8_t {
    InvalidRoutes, InvalidEvent, CreateDirectory, Open, Write, Replace
};

[[nodiscard]] inline std::string projection_json_quote(std::string_view value) {
    std::string out{"\""};
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] inline std::string projection_broker_name(broker_view::BrokerId broker) {
    return broker == broker_view::BrokerId::Fyers ? "FYERS" : "KITE";
}

[[nodiscard]] inline std::string projection_kind(broker_view::BrokerLogKind kind) {
    using broker_view::BrokerLogKind;
    switch (kind) {
    case BrokerLogKind::Auth: return "auth";
    case BrokerLogKind::AccountSnapshot: return "account";
    case BrokerLogKind::Feed: return "feed";
    case BrokerLogKind::Route: return "route";
    case BrokerLogKind::OrderGate: return "order_gate";
    case BrokerLogKind::Error: return "error";
    }
    return "error";
}

[[nodiscard]] inline std::string projection_source(broker_view::BrokerLogKind kind) {
    using broker_view::BrokerLogKind;
    switch (kind) {
    case BrokerLogKind::Auth: return "auth_service";
    case BrokerLogKind::AccountSnapshot: return "account_service";
    case BrokerLogKind::Feed: return "feed_service";
    case BrokerLogKind::Route: return "route_service";
    case BrokerLogKind::OrderGate: return "oms_gate";
    case BrokerLogKind::Error: return "broker_service";
    }
    return "broker_service";
}

[[nodiscard]] inline std::string projection_event_id(broker_view::BrokerEventId id) {
    return std::to_string(id.high) + ':' + std::to_string(id.low);
}

template <class Service>
[[nodiscard]] inline bool projection_events(const Service& service,
                                            std::ofstream& out) {
    out << "{\"broker\":" << projection_json_quote(
        projection_broker_name(service.provider()))
        << ",\"omitted_events\":" << service.omitted_logs() << ",\"events\":[";
    for (std::size_t i = 0; i < service.log_size(); ++i) {
        const auto* event = service.log_at(i);
        if (event == nullptr || !broker_view::valid(*event)) return false;
        if (i != 0) out << ',';
        const auto occurred = event->occurred_at.ns_since_epoch() / 1'000'000'000LL;
        out << "{\"event_id\":" << projection_json_quote(projection_event_id(event->event_id))
            << ",\"occurred_at_unix\":" << occurred
            << ",\"source\":" << projection_json_quote(projection_source(event->kind))
            << ",\"kind\":" << projection_json_quote(projection_kind(event->kind))
            << ",\"message\":" << projection_json_quote(event->message.view());
        if (event->request_id.has_value()) {
            out << ",\"correlation_id\":"
                << projection_json_quote(projection_event_id(*event->request_id));
        }
        out << '}';
    }
    out << "]}";
    return static_cast<bool>(out);
}

/// Publish both providers' bounded logs and the explicit route choice.
/// `now` and `ttl_seconds` are supplied by the supervisor, making expiry
/// deterministic in tests and preventing a stale file from looking live.
template <class FyersService, class KiteService>
[[nodiscard]] inline std::expected<void, ActivityProjectionError>
publish_activity_projection(const std::filesystem::path& destination,
                             const broker_view::Routes& routes,
                             const FyersService& fyers,
                             const KiteService& kite,
                             Timestamp now,
                             std::int64_t ttl_seconds = 10) {
    if (!broker_view::valid(routes.data_primary)
        || !broker_view::valid(routes.data_fallback)
        || !broker_view::valid(routes.order_primary)
        || routes.revision == 0 || ttl_seconds <= 0 || now <= Timestamp::epoch())
        return std::unexpected(ActivityProjectionError::InvalidRoutes);
    std::error_code ec;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) return std::unexpected(ActivityProjectionError::CreateDirectory);
    const auto temporary = destination.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(ActivityProjectionError::Open);
        const auto observed = now.ns_since_epoch() / 1'000'000'000LL;
        out << "{\"schema_version\":1,\"observed_at_unix\":" << observed
            << ",\"expires_at_unix\":" << observed + ttl_seconds
            << ",\"routes\":{\"data_primary\":"
            << projection_json_quote(projection_broker_name(routes.data_primary))
            << ",\"data_fallback\":"
            << projection_json_quote(projection_broker_name(routes.data_fallback))
            << ",\"order_primary\":"
            << projection_json_quote(projection_broker_name(routes.order_primary))
            << ",\"mode\":"
            << projection_json_quote(routes.mode == broker_view::TradingMode::Paper ? "PAPER"
                                      : routes.mode == broker_view::TradingMode::LiveDisabled ? "LIVE_DISABLED"
                                                                                              : "LIVE_ARMED")
            << ",\"revision\":" << routes.revision << "},\"brokers\":[";
        if (!projection_events(fyers, out)) return std::unexpected(ActivityProjectionError::InvalidEvent);
        out << ',';
        if (!projection_events(kite, out)) return std::unexpected(ActivityProjectionError::InvalidEvent);
        out << "]}\n";
        if (!out) return std::unexpected(ActivityProjectionError::Write);
    }
    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return std::unexpected(ActivityProjectionError::Replace);
    }
    return {};
}

/// One bounded supervisor step. The application loop calls this at its chosen
/// cadence (typically once per poll interval); both services are pumped before
/// one atomic projection is replaced, so the desktop never combines files from
/// different service observations.
template <class FyersService, class KiteService>
class BrokerActivitySupervisor {
public:
    BrokerActivitySupervisor(FyersService& fyers, KiteService& kite,
                             std::filesystem::path destination,
                             broker_view::Routes routes) noexcept
        : fyers_(fyers), kite_(kite), destination_(std::move(destination)),
          routes_(routes) {}

    /// Run one service cycle at explicit UTC nanoseconds. No wall-clock read is
    /// performed here; the owner supplies `now` for deterministic replay/tests.
    [[nodiscard]] std::expected<void, ActivityProjectionError>
    step(Timestamp now, std::int64_t ttl_seconds = 10) {
        (void)fyers_.pump(now);
        (void)kite_.pump(now);
        return publish_activity_projection(destination_, routes_, fyers_, kite_,
                                           now, ttl_seconds);
    }

    void set_routes(broker_view::Routes routes) noexcept { routes_ = routes; }

private:
    FyersService& fyers_;
    KiteService& kite_;
    std::filesystem::path destination_;
    broker_view::Routes routes_{};
};

} // namespace altair::app