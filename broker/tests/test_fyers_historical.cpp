#include <broker/fyers_historical.hpp>

#include <cstdio>

int main() {
    using namespace altair::fyers_history;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const auto request = uri("NSE:SBIN-EQ", "1", "2026-09-01", "2026-09-02");
    check(request && request->find("/data/history?symbol=NSE:SBIN-EQ") == 0
              && request->find("resolution=1") != std::string::npos
              && request->find("date_format=1") != std::string::npos,
          "FYERS v3 one-minute history URI is explicit");
    check(!uri("NSE:BAD&x=1", "1", "2026-09-01", "2026-09-02"),
          "query injection in symbol is refused");
    check(!uri("NSE:SBIN-EQ", "2", "2026-09-01", "2026-09-02"),
          "unsupported resolution is refused");
    const auto minute_chunks = chunk_requests("1", "2026-01-01", "2026-09-28");
    check(minute_chunks && minute_chunks->size() == 3
              && minute_chunks->front().from == "2026-01-01"
              && minute_chunks->front().to == "2026-04-10"
              && minute_chunks->back().to == "2026-09-28",
          "minute history is split into documented 100-day inclusive windows");
    const auto daily_chunks = chunk_requests("D", "2025-01-01", "2026-09-28");
    check(daily_chunks && daily_chunks->size() == 2,
          "daily history is split into documented 366-day windows");
    const auto candles = parse(
        R"({"s":"ok","candles":[[1788234300,250.00,251.05,249.95,250.50,1000],[1788234360,250.50,251.10,250.10,250.90,500]]})");
    check(candles && candles->size() == 2, "official epoch-array candle shape parses");
    check(parse(R"({"candles":[[1788234300,250,251,249,250.5,1000]],"code":200,"message":"","s":"ok"})").has_value(),
          "top-level FYERS fields parse regardless of JSON member order");
    if (candles) {
        check((*candles)[0].ts_ns == 1'788'234'300'000'000'000LL
                  && (*candles)[0].open == 250.0 && (*candles)[0].volume == 1000.0,
              "timestamp, OHLC and volume retain provider values");
    }
    check(!parse(R"({"s":"error","message":"invalid"})"),
          "API error is not an empty trading day");
    check(!parse(R"({"s":"ok","candles":[[1,10,9,8,10,1]]})"),
          "invalid OHLC range is refused");
    check(!parse(R"({"s":"ok","candles":[[2,1,2,1,1,1],[1,1,2,1,1,1]]})"),
          "nonascending timestamps are refused");
    check(!parse(R"({"s":"ok","candles":[[1,1,2,1,1]]})"),
          "short candle is refused");
    return failures == 0 ? 0 : 1;
}
