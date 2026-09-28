// oms/fyers_adapter.hpp
// P7-09. FYERS API v3 order vocabulary, translation only — no transport.
#pragma once

#include <oms/order_state.hpp>
#include <risk/cost.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>

namespace altair::oms {

enum class FyersAdapterError : std::uint8_t {
    MalformedIntent, UnrepresentablePrice, UnsupportedProduct,
    UnknownStatus, MalformedResponse
};

enum class FyersOrderType : std::uint8_t { Limit = 1, Market = 2, StopMarket = 3, StopLimit = 4 };

struct FyersOrderIntent {
    std::string_view symbol;
    Qty quantity{};
    Side side{Side::Buy};
    FyersOrderType type{FyersOrderType::Limit};
    std::string_view product_type{"INTRADAY"};
    Price limit_price{};
    Price stop_price{};
    std::string_view validity{"DAY"};
    std::string_view order_tag;
    bool offline_order{};
};

namespace fyers_detail {
inline void json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    static constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 0x0F]);
            } else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

[[nodiscard]] inline std::expected<std::string, FyersAdapterError>
price_json(Price price) {
    if (price.raw() < 0) return std::unexpected(FyersAdapterError::UnrepresentablePrice);
    char buffer[48];
    const int n = std::snprintf(buffer, sizeof buffer, "%lld.%02lld",
        static_cast<long long>(price.raw() / 100),
        static_cast<long long>(price.raw() % 100));
    if (n <= 0 || n >= static_cast<int>(sizeof buffer))
        return std::unexpected(FyersAdapterError::UnrepresentablePrice);
    return std::string(buffer, static_cast<std::size_t>(n));
}

[[nodiscard]] inline bool product_supported(std::string_view p) noexcept {
    return p == "CNC" || p == "INTRADAY" || p == "MARGIN" || p == "MTF";
}
} // namespace fyers_detail

/// Exact body for POST /api/v3/orders/sync. Optional TP/SL overlays, basket,
/// smart and GTT orders are separate future cards and cannot appear by default.
[[nodiscard]] inline std::expected<std::string, FyersAdapterError>
build_fyers_place_order(const FyersOrderIntent& order) {
    if (order.symbol.empty() || order.quantity.raw() <= 0
        || (order.validity != "DAY" && order.validity != "IOC"))
        return std::unexpected(FyersAdapterError::MalformedIntent);
    if (!fyers_detail::product_supported(order.product_type))
        return std::unexpected(FyersAdapterError::UnsupportedProduct);
    const bool needs_limit = order.type == FyersOrderType::Limit
                          || order.type == FyersOrderType::StopLimit;
    const bool needs_stop = order.type == FyersOrderType::StopMarket
                         || order.type == FyersOrderType::StopLimit;
    if ((needs_limit && order.limit_price.raw() <= 0)
        || (needs_stop && order.stop_price.raw() <= 0))
        return std::unexpected(FyersAdapterError::MalformedIntent);
    if (!needs_limit && order.limit_price.raw() != 0)
        return std::unexpected(FyersAdapterError::MalformedIntent);

    const auto limit = fyers_detail::price_json(order.limit_price);
    const auto stop = fyers_detail::price_json(order.stop_price);
    if (!limit || !stop) return std::unexpected(FyersAdapterError::UnrepresentablePrice);
    std::string body{"{"};
    const auto field_string = [&](std::string_view key, std::string_view value) {
        if (body.size() > 1) body.push_back(',');
        fyers_detail::json_string(body, key); body.push_back(':');
        fyers_detail::json_string(body, value);
    };
    const auto field_raw = [&](std::string_view key, std::string_view value) {
        if (body.size() > 1) body.push_back(',');
        fyers_detail::json_string(body, key); body.push_back(':'); body.append(value);
    };
    field_string("symbol", order.symbol);
    field_raw("qty", std::to_string(order.quantity.raw()));
    field_raw("type", std::to_string(static_cast<unsigned>(order.type)));
    field_raw("side", order.side == Side::Buy ? "1" : "-1");
    field_string("productType", order.product_type);
    field_raw("limitPrice", *limit);
    field_raw("stopPrice", *stop);
    field_raw("disclosedQty", "0");
    field_string("validity", order.validity);
    field_raw("offlineOrder", order.offline_order ? "true" : "false");
    field_raw("stopLoss", "0");
    field_raw("takeProfit", "0");
    if (!order.order_tag.empty()) field_string("orderTag", order.order_tag);
    field_raw("isSliceOrder", "false");
    body.push_back('}');
    return body;
}

/// FYERS v3 documented order states: 1 cancelled, 2 traded, 4 transit,
/// 5 rejected, 6 pending, 7 expired. Reserved/unknown values refuse.
[[nodiscard]] inline std::expected<OrderState, FyersAdapterError>
map_fyers_status(int status) noexcept {
    switch (status) {
    case 1: return OrderState::Cancelled;
    case 2: return OrderState::Filled;
    case 4: return OrderState::PendingNew;
    case 5: return OrderState::Rejected;
    case 6: return OrderState::Open;
    case 7: return OrderState::Expired;
    default: return std::unexpected(FyersAdapterError::UnknownStatus);
    }
}

[[nodiscard]] inline std::expected<std::string, FyersAdapterError>
parse_fyers_order_id(std::string_view body) {
    for (const std::string_view key : {std::string_view{"\"id\""},
                                       std::string_view{"\"order_id\""}}) {
        const std::size_t p = body.find(key);
        if (p == std::string_view::npos) continue;
        std::size_t at = body.find(':', p + key.size());
        if (at == std::string_view::npos) continue;
        ++at; while (at < body.size() && (body[at] == ' ' || body[at] == '"')) ++at;
        std::string id;
        while (at < body.size() && body[at] != '"' && body[at] != ',' && body[at] != '}')
            id.push_back(body[at++]);
        if (!id.empty()) return id;
    }
    return std::unexpected(FyersAdapterError::MalformedResponse);
}

enum class FyersDispatchDisposition : std::uint8_t {
    Accepted, Rejected, ReconcileRequired
};

/// A timeout, HTTP 201/no acknowledgement, or malformed success is uncertain:
/// query the order book by tag/id. It is never safe for an automatic retry.
[[nodiscard]] constexpr FyersDispatchDisposition
fyers_dispatch_disposition(int http_status, bool acknowledged) noexcept {
    if (http_status == 200 && acknowledged) return FyersDispatchDisposition::Accepted;
    if (http_status >= 400 && http_status < 500 && http_status != 408 && http_status != 429)
        return FyersDispatchDisposition::Rejected;
    return FyersDispatchDisposition::ReconcileRequired;
}

} // namespace altair::oms
