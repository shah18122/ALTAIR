// broker/fyers_historical.hpp -- FYERS v3 history request/response boundary.
#pragma once

#include <broker/kite_historical.hpp> // RawCandle and date/time utilities

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace altair::fyers_history {

enum class Error : std::uint8_t {
    InvalidSymbol, InvalidResolution, InvalidRange, Malformed, ApiError, BadField
};

[[nodiscard]] inline bool valid_symbol(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    for (const unsigned char c : value)
        if (c < 0x21 || c > 0x7e || c == '&' || c == '=' || c == '?' || c == '#')
            return false;
    return true;
}

[[nodiscard]] inline bool valid_resolution(std::string_view value) noexcept {
    return value == "1" || value == "3" || value == "5" || value == "10"
        || value == "15" || value == "30" || value == "60" || value == "120"
        || value == "240" || value == "D";
}

/// FYERS documents a maximum of 100 days for minute resolutions and 366 days
/// for daily history. Split inclusively so a valid long request never depends
/// on undocumented truncation at the provider boundary.
[[nodiscard]] inline std::expected<std::vector<Chunk>, Error>
chunk_requests(std::string_view resolution, std::string_view from,
               std::string_view to) {
    if (!valid_resolution(resolution)) return std::unexpected(Error::InvalidResolution);
    std::int64_t first = 0, last = 0;
    if (!parse_date(from, first) || !parse_date(to, last) || last < first)
        return std::unexpected(Error::InvalidRange);
    const std::int64_t days = resolution == "D" ? 366 : 100;
    std::vector<Chunk> out;
    for (std::int64_t at = first; at <= last;) {
        const std::int64_t end = std::min(last, at + days - 1);
        out.push_back(Chunk{date_string(at), date_string(end)});
        at = end + 1;
    }
    return out;
}

/// FYERS date_format=1 uses YYYY-MM-DD and returns epoch-second candles.
[[nodiscard]] inline std::expected<std::string, Error>
uri(std::string_view symbol, std::string_view resolution,
    std::string_view from, std::string_view to, bool continuous = false) {
    std::int64_t from_day = 0, to_day = 0;
    if (!valid_symbol(symbol)) return std::unexpected(Error::InvalidSymbol);
    if (!valid_resolution(resolution)) return std::unexpected(Error::InvalidResolution);
    if (!parse_date(from, from_day) || !parse_date(to, to_day) || to_day < from_day)
        return std::unexpected(Error::InvalidRange);
    std::string result{"/data/history?symbol="};
    result.append(symbol);
    result += "&resolution=";
    result.append(resolution);
    result += "&date_format=1&range_from=";
    result.append(from);
    result += "&range_to=";
    result.append(to);
    result += "&cont_flag=";
    result += continuous ? '1' : '0';
    return result;
}

namespace detail {
inline void ws(std::string_view text, std::size_t& at) noexcept {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t'
           || text[at] == '\r' || text[at] == '\n')) ++at;
}
inline bool number(std::string_view text, std::size_t& at, double& value) {
    ws(text, at);
    const std::size_t begin = at;
    if (at < text.size() && (text[at] == '-' || text[at] == '+')) ++at;
    bool digit = false;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        ++at; digit = true;
    }
    if (at < text.size() && text[at] == '.') {
        ++at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            ++at; digit = true;
        }
    }
    if (!digit || at - begin > 40) return false;
    const std::string token{text.substr(begin, at - begin)};
    char* end = nullptr;
    value = std::strtod(token.c_str(), &end);
    return end == token.c_str() + token.size() && std::isfinite(value);
}
} // namespace detail

/// Parse {"s":"ok","candles":[[epoch_s,o,h,l,c,v], ...]}.
[[nodiscard]] inline std::expected<std::vector<RawCandle>, Error>
parse(std::string_view body) {
    const auto status = body.find("\"s\"");
    if (status == std::string_view::npos) return std::unexpected(Error::Malformed);
    const auto colon = body.find(':', status + 3);
    if (colon == std::string_view::npos) return std::unexpected(Error::Malformed);
    const auto ok = body.find("\"ok\"", colon + 1);
    if (ok == std::string_view::npos || ok > colon + 16)
        return std::unexpected(Error::ApiError);
    // JSON object member order is not significant. Live FYERS responses may
    // place candles before code/message/status, so never search one key only
    // after another key's byte position.
    const auto key = body.find("\"candles\"");
    if (key == std::string_view::npos) return std::unexpected(Error::Malformed);
    std::size_t at = body.find('[', key + 9);
    if (at == std::string_view::npos) return std::unexpected(Error::Malformed);
    ++at;
    std::vector<RawCandle> output;
    for (;;) {
        detail::ws(body, at);
        if (at >= body.size()) return std::unexpected(Error::Malformed);
        if (body[at] == ']') { ++at; break; }
        if (body[at] == ',') { ++at; detail::ws(body, at); }
        if (at >= body.size() || body[at++] != '[')
            return std::unexpected(Error::Malformed);
        double field[6]{};
        for (int i = 0; i < 6; ++i) {
            if (!detail::number(body, at, field[i]) || !std::isfinite(field[i]))
                return std::unexpected(Error::BadField);
            detail::ws(body, at);
            if (i != 5) {
                if (at >= body.size() || body[at++] != ',')
                    return std::unexpected(Error::Malformed);
            }
        }
        detail::ws(body, at);
        if (at >= body.size() || body[at++] != ']')
            return std::unexpected(Error::Malformed);
        if (field[0] <= 0 || std::floor(field[0]) != field[0]
            || field[0] > 9'223'372'036.0 || field[1] <= 0 || field[2] <= 0
            || field[3] <= 0 || field[4] <= 0 || field[5] < 0
            || std::floor(field[5]) != field[5]
            || field[2] < std::max(field[1], field[4])
            || field[3] > std::min(field[1], field[4]))
            return std::unexpected(Error::BadField);
        RawCandle candle{};
        candle.ts_ns = static_cast<std::int64_t>(field[0]) * 1'000'000'000LL;
        candle.open = field[1]; candle.high = field[2]; candle.low = field[3];
        candle.close = field[4]; candle.volume = field[5];
        output.push_back(candle);
    }
    detail::ws(body, at);
    // The documented response also carries code/message fields. They may
    // follow candles; accept the remaining top-level members without treating
    // their order as part of the wire contract. Still require a closing object
    // and reject a second array, which would make the candle boundary unclear.
    const auto close_object = body.find_last_not_of(" \t\r\n");
    if (close_object == std::string_view::npos || body[close_object] != '}'
        || (at < close_object && body[at] != ',')
        || body.find('[', at) != std::string_view::npos)
        return std::unexpected(Error::Malformed);
    for (std::size_t i = 1; i < output.size(); ++i)
        if (output[i].ts_ns <= output[i - 1].ts_ns)
            return std::unexpected(Error::BadField);
    return output;
}

} // namespace altair::fyers_history
