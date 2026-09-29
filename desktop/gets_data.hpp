// desktop/gets_data.hpp -- FYERS account and quote files, read for the
// GETS-style Terminal screens.
//
// Two helpers write what this reads: altair_fyers_account
// (data/fyers_account.json: funds, positions, orders, trade book) and
// altair_fyers_quotes (data/fyers_quotes.json: LTP, change, OHLC). Both run
// outside this process; desktop/ may not link broker/, and no credential
// enters this address space.
//
// This file turns their raw FYERS bodies into typed rows, resolves each
// ticker (instruments/fyers_symbol.hpp, then the Kite master for the dates a
// monthly ticker does not carry), and builds:
//   * the engine's BookLeg list for the Greek screens (risk/option_book.hpp);
//   * a typed broker_view::AccountSnapshot, so the Terminal's existing
//     positions and funds tables finally receive FYERS data. The typed
//     snapshot parser leaves FYERS positions untyped and reads funds under
//     names FYERS does not use (it answers "fund_limit" rows by id).
//
// MONEY. FYERS sends JSON numbers. A PRICE is accepted only when it is whole
// paise (within 1e-3 paisa of the double); anything finer is refused, not
// rounded. Averages and P&L are the broker's own arithmetic and may carry
// fractions of a paisa; those are rounded half away from zero and the column
// says "broker value".
#pragma once

#include "data/master_lookup.hpp"
#include "position_table.hpp"

#include <broker/account_snapshot.hpp>
#include <instruments/fyers_symbol.hpp>
#include <risk/option_book.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace altair::ui {

// ---- numbers ---------------------------------------------------------------

/// A price in whole paise, or nothing. Accepts a JSON number or numeric text.
[[nodiscard]] inline std::optional<std::int64_t> gets_price_paise(const QJsonValue& v) {
    double x = 0.0;
    if (v.isDouble()) {
        x = v.toDouble();
    } else if (v.isString()) {
        bool ok = false;
        x = v.toString().toDouble(&ok);
        if (!ok) return std::nullopt;
    } else {
        return std::nullopt;
    }
    const double scaled = x * 100.0;
    if (!std::isfinite(scaled) || std::fabs(scaled) > 9.0e15) return std::nullopt;
    const double r = std::round(scaled);
    if (std::fabs(scaled - r) > 1e-3) return std::nullopt;   // sub-paisa: refused
    return static_cast<std::int64_t>(r);
}

/// A broker-computed amount (average, P&L) rounded to the paisa.
[[nodiscard]] inline std::optional<std::int64_t> gets_amount_paise(const QJsonValue& v) {
    double x = 0.0;
    if (v.isDouble()) {
        x = v.toDouble();
    } else if (v.isString()) {
        bool ok = false;
        x = v.toString().toDouble(&ok);
        if (!ok) return std::nullopt;
    } else {
        return std::nullopt;
    }
    const double scaled = x * 100.0;
    if (!std::isfinite(scaled) || std::fabs(scaled) > 9.0e15) return std::nullopt;
    return static_cast<std::int64_t>(std::round(scaled));
}

[[nodiscard]] inline std::optional<std::int64_t> gets_integer(const QJsonValue& v) {
    if (v.isDouble()) {
        const double x = v.toDouble();
        if (!std::isfinite(x) || std::fabs(x) > 9.0e15 || x != std::floor(x)) return std::nullopt;
        return static_cast<std::int64_t>(x);
    }
    if (v.isString()) {
        bool ok = false;
        const qlonglong n = v.toString().toLongLong(&ok);
        if (ok) return static_cast<std::int64_t>(n);
    }
    return std::nullopt;
}

/// FYERS order/trade time, "04-Aug-2023 10:40:39", in IST.
[[nodiscard]] inline QDateTime gets_ist_time(const QString& text) {
    QDateTime t = QDateTime::fromString(text, QStringLiteral("dd-MMM-yyyy HH:mm:ss"));
    if (!t.isValid()) return {};
    t.setTimeZone(QTimeZone(19800));
    return t;
}

// ---- the account file ------------------------------------------------------

struct GetsFund {
    int id{};
    QString title;
    std::optional<std::int64_t> equity;      ///< paise
    std::optional<std::int64_t> commodity;   ///< paise
};

struct GetsPosition {
    QString symbol;
    QString product;
    std::int64_t net_qty{};                  ///< signed units
    std::optional<std::int64_t> net_avg;     ///< paise, broker value
    std::optional<std::int64_t> ltp;         ///< paise
    std::int64_t buy_qty{};
    std::int64_t sell_qty{};
    std::optional<std::int64_t> buy_avg;
    std::optional<std::int64_t> sell_avg;
    std::optional<std::int64_t> realized;    ///< paise, broker value
    std::optional<std::int64_t> unrealized;
};

enum class GetsOrderStatus : int {
    Cancelled = 1, Filled = 2, Transit = 4, Rejected = 5, Pending = 6, Expired = 7
};

struct GetsOrder {
    QString id;
    QString symbol;
    int status{};
    int side{};                              ///< 1 buy, -1 sell
    int type{};                              ///< 1 limit, 2 market, 3 SL-M, 4 SL-L
    std::int64_t qty{};
    std::int64_t filled_qty{};
    std::optional<std::int64_t> limit_price;
    std::optional<std::int64_t> traded_price;
    QString product;
    QDateTime at;
    QString message;
};

struct GetsTrade {
    QString symbol;
    int side{};                              ///< 1 buy, -1 sell
    std::int64_t qty{};
    std::optional<std::int64_t> price;       ///< paise
    std::optional<std::int64_t> value;       ///< paise
    QString product;
    QString order_number;
    QString trade_number;
    QDateTime at;
};

struct GetsAccount {
    bool loaded{};
    QString error;
    qint64 fetched_at{};                     ///< unix seconds
    qint64 expires_at{};
    QString account_id;
    bool complete{};
    int funds_status{}, positions_status{}, orders_status{}, tradebook_status{};
    bool funds_ok{}, positions_ok{}, orders_ok{}, trades_ok{};
    std::vector<GetsFund> funds;
    std::vector<GetsPosition> positions;
    std::vector<GetsOrder> orders;
    std::vector<GetsTrade> trades;
    QStringList warnings;                    ///< rows refused, with the reason
};

namespace gets_detail {

[[nodiscard]] inline QJsonArray section_array(const QJsonObject& root, const char* section,
                                              const char* list, bool& ok) {
    const QJsonValue body = root.value(QLatin1String(section));
    ok = body.isObject() && body.toObject().value(QStringLiteral("s")).toString() == QStringLiteral("ok");
    if (!ok) return {};
    const QJsonValue rows = body.toObject().value(QLatin1String(list));
    if (!rows.isArray()) { ok = rows.isNull() || rows.isUndefined(); return {}; }
    return rows.toArray();
}

} // namespace gets_detail

[[nodiscard]] inline GetsAccount parse_gets_account(const QByteArray& raw) {
    GetsAccount a;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        a.error = QStringLiteral("the account file is not valid JSON");
        return a;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != 1
        || root.value(QStringLiteral("broker")).toString() != QStringLiteral("FYERS")) {
        a.error = QStringLiteral("not a FYERS account snapshot (schema 1)");
        return a;
    }
    a.fetched_at = root.value(QStringLiteral("fetched_at_unix")).toInteger();
    a.expires_at = root.value(QStringLiteral("account_expires_at_unix")).toInteger();
    a.account_id = root.value(QStringLiteral("account_id")).toString();
    a.complete = root.value(QStringLiteral("snapshot_complete")).toBool();
    a.funds_status = root.value(QStringLiteral("funds_status")).toInt();
    a.positions_status = root.value(QStringLiteral("positions_status")).toInt();
    a.orders_status = root.value(QStringLiteral("orders_status")).toInt();
    a.tradebook_status = root.value(QStringLiteral("tradebook_status")).toInt();
    if (a.fetched_at <= 0 || a.account_id.isEmpty()) {
        a.error = QStringLiteral("the account file has no fetch time or account id");
        return a;
    }

    for (const QJsonValue v : gets_detail::section_array(root, "funds", "fund_limit", a.funds_ok)) {
        const QJsonObject o = v.toObject();
        GetsFund f;
        f.id = o.value(QStringLiteral("id")).toInt();
        f.title = o.value(QStringLiteral("title")).toString();
        f.equity = gets_amount_paise(o.value(QStringLiteral("equityAmount")));
        f.commodity = gets_amount_paise(o.value(QStringLiteral("commodityAmount")));
        a.funds.push_back(f);
    }
    for (const QJsonValue v : gets_detail::section_array(root, "positions", "netPositions", a.positions_ok)) {
        const QJsonObject o = v.toObject();
        GetsPosition p;
        p.symbol = o.value(QStringLiteral("symbol")).toString();
        p.product = o.value(QStringLiteral("productType")).toString();
        const auto net = gets_integer(o.value(QStringLiteral("netQty")));
        if (p.symbol.isEmpty() || !net) {
            a.warnings << QStringLiteral("position row without symbol or net quantity skipped");
            continue;
        }
        p.net_qty = *net;
        // FYERS reports side -1 for a short; guard against an unsigned netQty.
        if (o.value(QStringLiteral("side")).toInt() == -1 && p.net_qty > 0) p.net_qty = -p.net_qty;
        p.net_avg = gets_amount_paise(o.value(QStringLiteral("netAvg")));
        p.ltp = gets_price_paise(o.value(QStringLiteral("ltp")));
        p.buy_qty = gets_integer(o.value(QStringLiteral("buyQty"))).value_or(0);
        p.sell_qty = gets_integer(o.value(QStringLiteral("sellQty"))).value_or(0);
        p.buy_avg = gets_amount_paise(o.value(QStringLiteral("buyAvg")));
        p.sell_avg = gets_amount_paise(o.value(QStringLiteral("sellAvg")));
        p.realized = gets_amount_paise(o.value(QStringLiteral("realized_profit")));
        p.unrealized = gets_amount_paise(o.value(QStringLiteral("unrealized_profit")));
        a.positions.push_back(p);
    }
    for (const QJsonValue v : gets_detail::section_array(root, "orders", "orderBook", a.orders_ok)) {
        const QJsonObject o = v.toObject();
        GetsOrder r;
        r.id = o.value(QStringLiteral("id")).toString();
        r.symbol = o.value(QStringLiteral("symbol")).toString();
        r.status = o.value(QStringLiteral("status")).toInt();
        r.side = o.value(QStringLiteral("side")).toInt();
        r.type = o.value(QStringLiteral("type")).toInt();
        r.qty = gets_integer(o.value(QStringLiteral("qty"))).value_or(0);
        r.filled_qty = gets_integer(o.value(QStringLiteral("filledQty"))).value_or(0);
        r.limit_price = gets_price_paise(o.value(QStringLiteral("limitPrice")));
        r.traded_price = gets_amount_paise(o.value(QStringLiteral("tradedPrice")));
        r.product = o.value(QStringLiteral("productType")).toString();
        r.at = gets_ist_time(o.value(QStringLiteral("orderDateTime")).toString());
        r.message = o.value(QStringLiteral("message")).toString();
        a.orders.push_back(r);
    }
    for (const QJsonValue v : gets_detail::section_array(root, "tradebook", "tradeBook", a.trades_ok)) {
        const QJsonObject o = v.toObject();
        GetsTrade t;
        t.symbol = o.value(QStringLiteral("symbol")).toString();
        t.side = o.value(QStringLiteral("side")).toInt();
        const auto qty = gets_integer(o.value(QStringLiteral("tradedQty")));
        if (t.symbol.isEmpty() || !qty || *qty <= 0 || (t.side != 1 && t.side != -1)) {
            a.warnings << QStringLiteral("trade row without symbol, side or quantity skipped");
            continue;
        }
        t.qty = *qty;
        t.price = gets_price_paise(o.value(QStringLiteral("tradePrice")));
        t.value = gets_amount_paise(o.value(QStringLiteral("tradeValue")));
        t.product = o.value(QStringLiteral("productType")).toString();
        t.order_number = o.value(QStringLiteral("orderNumber")).toString();
        t.trade_number = o.value(QStringLiteral("tradeNumber")).toString();
        t.at = gets_ist_time(o.value(QStringLiteral("orderDateTime")).toString());
        a.trades.push_back(t);
    }
    a.loaded = true;
    return a;
}

[[nodiscard]] inline GetsAccount load_gets_account(const QString& path) {
    QFile f(path);
    constexpr qint64 kMaxBytes = 16 * 1024 * 1024;
    if (!f.open(QIODevice::ReadOnly)) {
        GetsAccount a;
        a.error = QStringLiteral("no FYERS account snapshot yet (%1)").arg(path);
        return a;
    }
    if (f.size() > kMaxBytes) {
        GetsAccount a;
        a.error = QStringLiteral("the FYERS account snapshot is larger than 16 MB; refused");
        return a;
    }
    return parse_gets_account(f.readAll());
}

/// A fund_limit row by FYERS id (1 total, 2 utilised, 3 clear balance,
/// 5 collateral, 10 available balance), equity segment.
[[nodiscard]] inline std::optional<std::int64_t> gets_fund(const GetsAccount& a, int id) {
    for (const auto& f : a.funds)
        if (f.id == id) return f.equity;
    return std::nullopt;
}

// ---- the quotes file -------------------------------------------------------

struct GetsQuote {
    QString symbol;
    bool ok{};
    QString error;
    QString description;
    std::optional<std::int64_t> ltp, change, open, high, low, prev_close, atp;
    std::optional<double> change_pct;
    std::optional<qint64> volume;
    qint64 time{};                           ///< unix seconds of the last trade
};

struct GetsQuotes {
    bool loaded{};
    QString error;
    qint64 fetched_at{};
    int requested{};
    int failed_responses{};
    QHash<QString, GetsQuote> by_symbol;
    QStringList order;                       ///< as returned
};

[[nodiscard]] inline GetsQuotes parse_gets_quotes(const QByteArray& raw) {
    GetsQuotes q;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        q.error = QStringLiteral("the quotes file is not valid JSON");
        return q;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != 1
        || root.value(QStringLiteral("broker")).toString() != QStringLiteral("FYERS")) {
        q.error = QStringLiteral("not a FYERS quotes snapshot (schema 1)");
        return q;
    }
    q.fetched_at = root.value(QStringLiteral("fetched_at_unix")).toInteger();
    q.requested = root.value(QStringLiteral("requested")).toInt();
    for (const QJsonValue r : root.value(QStringLiteral("responses")).toArray()) {
        const QJsonObject response = r.toObject();
        const QJsonValue body = response.value(QStringLiteral("body"));
        if (!body.isObject()) { ++q.failed_responses; continue; }
        for (const QJsonValue item : body.toObject().value(QStringLiteral("d")).toArray()) {
            const QJsonObject o = item.toObject();
            GetsQuote g;
            g.symbol = o.value(QStringLiteral("n")).toString();
            if (g.symbol.isEmpty()) continue;
            const QJsonObject v = o.value(QStringLiteral("v")).toObject();
            g.ok = o.value(QStringLiteral("s")).toString() == QStringLiteral("ok");
            if (!g.ok) {
                g.error = v.value(QStringLiteral("errmsg")).toString();
                if (g.error.isEmpty()) g.error = QStringLiteral("rejected by FYERS");
            } else {
                g.description = v.value(QStringLiteral("description")).toString();
                g.ltp = gets_price_paise(v.value(QStringLiteral("lp")));
                g.change = gets_amount_paise(v.value(QStringLiteral("ch")));
                g.open = gets_price_paise(v.value(QStringLiteral("open_price")));
                g.high = gets_price_paise(v.value(QStringLiteral("high_price")));
                g.low = gets_price_paise(v.value(QStringLiteral("low_price")));
                g.prev_close = gets_price_paise(v.value(QStringLiteral("prev_close_price")));
                g.atp = gets_amount_paise(v.value(QStringLiteral("atp")));
                const QJsonValue chp = v.value(QStringLiteral("chp"));
                if (chp.isDouble() && std::isfinite(chp.toDouble())) g.change_pct = chp.toDouble();
                if (const auto vol = gets_integer(v.value(QStringLiteral("volume")))) g.volume = *vol;
                g.time = gets_integer(v.value(QStringLiteral("tt"))).value_or(0);
            }
            if (!q.by_symbol.contains(g.symbol)) q.order << g.symbol;
            q.by_symbol.insert(g.symbol, g);
        }
    }
    q.loaded = true;
    return q;
}

[[nodiscard]] inline GetsQuotes load_gets_quotes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        GetsQuotes q;
        q.error = QStringLiteral("no FYERS quotes yet (%1)").arg(path);
        return q;
    }
    if (f.size() > 16 * 1024 * 1024) {
        GetsQuotes q;
        q.error = QStringLiteral("the FYERS quotes file is larger than 16 MB; refused");
        return q;
    }
    return parse_gets_quotes(f.readAll());
}

// ---- tickers -> instruments --------------------------------------------------

struct GetsInstrument {
    QString symbol;
    std::optional<instruments::FyersSymbol> parsed;
    QString spot_symbol;                     ///< empty when no spot can be named
    std::optional<Timestamp> expiry;
    QString expiry_source;                   ///< "master", "ticker" or empty
    std::optional<std::int64_t> lot;
    Segment segment{Segment::Cash};
    Exchange exchange{Exchange::NSE};
};

[[nodiscard]] inline GetsInstrument
resolve_gets_instrument(const QString& symbol, const MasterIndex* master, int reference_year) {
    using instruments::FyersSymbolKind;
    GetsInstrument g;
    g.symbol = symbol;
    const QByteArray utf8 = symbol.toUtf8();
    const auto parsed = instruments::parse_fyers_symbol(
        std::string_view{utf8.constData(), static_cast<std::size_t>(utf8.size())}, reference_year);
    if (!parsed) return g;
    g.parsed = *parsed;
    g.spot_symbol = QString::fromStdString(instruments::fyers_spot_symbol(*parsed));
    const QString ex = QString::fromLatin1(parsed->exchange.data());
    g.exchange = ex == QStringLiteral("BSE") ? Exchange::BSE : Exchange::NSE;
    const bool derivative = parsed->kind == FyersSymbolKind::Future
                         || parsed->kind == FyersSymbolKind::Option;
    g.segment = parsed->kind == FyersSymbolKind::Option ? Segment::Opt
              : parsed->kind == FyersSymbolKind::Future ? Segment::Fut
              : ex == QStringLiteral("MCX") ? Segment::Commodity : Segment::Cash;
    if (master != nullptr && master->loaded()) {
        const QString body = symbol.section(QLatin1Char(':'), 1);
        const QString kite_exchange = ex == QStringLiteral("MCX") ? QStringLiteral("MCX")
            : derivative ? (g.exchange == Exchange::BSE ? QStringLiteral("BFO") : QStringLiteral("NFO"))
                         : ex;
        const QString kite_symbol = derivative ? body : body.section(QLatin1Char('-'), 0, -2);
        const InstrumentProfile p = master->find_symbol(kite_exchange, kite_symbol);
        if (p.found) {
            if (p.lot_size > 0) g.lot = p.lot_size;
            if (derivative && p.expiry_ns > 0) {
                // The Kite master dates expiry at IST midnight; trading stops at 15:30.
                constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
                constexpr std::int64_t kCloseNs = (15 * 3600 + 30 * 60) * 1'000'000'000LL;
                const std::int64_t into_day = ((p.expiry_ns + kIstOffset.raw()) % kDayNs + kDayNs) % kDayNs;
                g.expiry = Timestamp{into_day == 0 ? p.expiry_ns + kCloseNs : p.expiry_ns};
                g.expiry_source = QStringLiteral("master");
            }
        }
    }
    if (!g.expiry && derivative) {
        if (const auto e = instruments::fyers_ticker_expiry(*parsed)) {
            g.expiry = *e;
            g.expiry_source = QStringLiteral("ticker");
        }
    }
    return g;
}

[[nodiscard]] inline broker_view::PositionProduct gets_product(const QString& p) {
    using broker_view::PositionProduct;
    if (p == QStringLiteral("CNC")) return PositionProduct::Delivery;
    if (p == QStringLiteral("INTRADAY") || p == QStringLiteral("CO") || p == QStringLiteral("BO"))
        return PositionProduct::Intraday;
    if (p == QStringLiteral("MARGIN")) return PositionProduct::CarryForward;
    if (p == QStringLiteral("MTF")) return PositionProduct::Margin;
    return PositionProduct::Unknown;
}

/// Stable, non-None instrument key for a ticker (FNV-1a 32).
[[nodiscard]] inline broker_view::InstrumentKey gets_instrument_key(const QString& symbol) {
    std::uint32_t h = 2166136261u;
    for (const char c : symbol.toUtf8()) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    if (h == static_cast<std::uint32_t>(broker_view::InstrumentKey::None)) h ^= 1u;
    return static_cast<broker_view::InstrumentKey>(h);
}

[[nodiscard]] inline InstrumentDisplay gets_display(const GetsInstrument& g) {
    using instruments::FyersSymbolKind;
    InstrumentDisplay d;
    d.exchange = g.parsed ? QString::fromLatin1(g.parsed->exchange.data()) : g.symbol.section(QLatin1Char(':'), 0, 0);
    d.symbol = g.symbol.section(QLatin1Char(':'), 1);
    d.segment = g.segment == Segment::Opt ? QStringLiteral("OPT")
              : g.segment == Segment::Fut ? QStringLiteral("FUT")
              : g.segment == Segment::Commodity ? QStringLiteral("COM") : QStringLiteral("CASH");
    if (g.expiry) {
        d.expiry = QDateTime::fromMSecsSinceEpoch(g.expiry->ns_since_epoch() / 1'000'000,
                                                  QTimeZone(19800)).date();
    }
    if (g.parsed && g.parsed->kind == FyersSymbolKind::Option) {
        d.strike = g.parsed->strike;
        d.option_type = g.parsed->right == instruments::FyersRight::Call ? QStringLiteral("CE")
                                                                        : QStringLiteral("PE");
    }
    if (g.lot) d.lot_size = LotSize{*g.lot};
    return d;
}

// ---- the book ----------------------------------------------------------------

/// Everything the Greek screens need, built once per refresh.
struct GetsBook {
    std::vector<GetsInstrument> instruments;     ///< index == BookLeg::id
    std::vector<BookLeg> legs;
    std::vector<BookLegView> views;
    std::vector<QString> underlyings;            ///< index == BookLeg::underlying (spot ticker)
    std::vector<std::optional<Price>> spots;     ///< per underlying
    std::vector<bool> watch_only;                ///< per leg: from the watch list, no position
    BookParams params;
};

[[nodiscard]] inline std::optional<Price> gets_quote_ltp(const GetsQuotes& q, const QString& symbol) {
    const auto it = q.by_symbol.constFind(symbol);
    if (it == q.by_symbol.constEnd() || !it->ok || !it->ltp || *it->ltp <= 0) return std::nullopt;
    return Price{*it->ltp};
}

/// Open positions (net != 0) and watch-list tickers, as book legs.
/// `user_iv` maps ticker -> user IV in percent. Rows that cannot be resolved
/// still become legs so the screen can say why they have no Greeks.
[[nodiscard]] inline GetsBook build_gets_book(const GetsAccount& account, const GetsQuotes& quotes,
                                              const QStringList& watch, const MasterIndex* master,
                                              const QHash<QString, double>& user_iv,
                                              const BookParams& params, int reference_year) {
    using instruments::FyersRight;
    using instruments::FyersSymbolKind;
    GetsBook b;
    b.params = params;
    const auto underlying_of = [&b, &quotes](const QString& spot_symbol) -> std::uint32_t {
        for (std::size_t i = 0; i < b.underlyings.size(); ++i)
            if (b.underlyings[i] == spot_symbol) return static_cast<std::uint32_t>(i);
        b.underlyings.push_back(spot_symbol);
        b.spots.push_back(spot_symbol.isEmpty() ? std::nullopt : gets_quote_ltp(quotes, spot_symbol));
        return static_cast<std::uint32_t>(b.underlyings.size() - 1);
    };
    const auto add = [&](const QString& symbol, std::int64_t units, std::optional<std::int64_t> avg,
                         std::optional<std::int64_t> position_ltp, bool watch_only) {
        GetsInstrument inst = resolve_gets_instrument(symbol, master, reference_year);
        BookLeg leg{};
        leg.id = static_cast<std::uint32_t>(b.instruments.size());
        leg.kind = !inst.parsed ? BookLegKind::Cash
                 : inst.parsed->kind == FyersSymbolKind::Option ? BookLegKind::Option
                 : inst.parsed->kind == FyersSymbolKind::Future ? BookLegKind::Future
                                                                : BookLegKind::Cash;
        if (inst.parsed && inst.parsed->kind == FyersSymbolKind::Option) {
            leg.right = inst.parsed->right == FyersRight::Call ? OptionRight::Call : OptionRight::Put;
            leg.strike = inst.parsed->strike;
        }
        leg.expiry_known = inst.expiry.has_value();
        if (inst.expiry) leg.expiry = *inst.expiry;
        leg.units = Qty{units};
        leg.average = Price{avg.value_or(0)};
        if (const auto q = gets_quote_ltp(quotes, symbol)) leg.mark = *q;
        else if (position_ltp && *position_ltp > 0) leg.mark = Price{*position_ltp};
        if (const auto it = user_iv.constFind(symbol); it != user_iv.constEnd() && *it > 0.0)
            leg.user_vol = Vol{*it / 100.0};
        if (inst.lot) leg.lot = LotSize{*inst.lot};
        const QString spot = leg.kind == BookLegKind::Cash ? symbol : inst.spot_symbol;
        leg.underlying = underlying_of(spot);
        b.instruments.push_back(inst);
        b.legs.push_back(leg);
        b.watch_only.push_back(watch_only);
    };
    for (const auto& p : account.positions)
        if (p.net_qty != 0) add(p.symbol, p.net_qty, p.net_avg, p.ltp, false);
    for (const QString& w : watch) {
        bool held = false;
        for (const auto& inst : b.instruments) held = held || inst.symbol == w;
        if (!held && !w.trimmed().isEmpty()) add(w.trimmed(), 0, std::nullopt, std::nullopt, true);
    }
    for (const auto& leg : b.legs) {
        const auto& spot = b.spots[leg.underlying];
        std::optional<Price> s = spot;
        if (leg.kind == BookLegKind::Cash && !s && leg.mark) s = leg.mark;
        b.views.push_back(value_book_leg(leg, s, params));
    }
    return b;
}

/// The tickers a quotes refresh must include for this account: every open
/// position, its underlying, and the watch list.
[[nodiscard]] inline QStringList gets_quote_symbols(const GetsAccount& account, const QStringList& watch,
                                                    int reference_year) {
    QStringList out;
    const auto push = [&out](const QString& s) { if (!s.isEmpty() && !out.contains(s)) out << s; };
    for (const auto& p : account.positions) {
        if (p.net_qty == 0) continue;
        push(p.symbol);
        push(resolve_gets_instrument(p.symbol, nullptr, reference_year).spot_symbol);
    }
    for (const QString& w : watch) {
        push(w.trimmed());
        push(resolve_gets_instrument(w.trimmed(), nullptr, reference_year).spot_symbol);
    }
    return out;
}

// ---- the typed snapshot for the Terminal's existing tables -------------------

struct GetsTypedAccount {
    std::optional<broker_view::AccountSnapshot> snapshot;
    QHash<std::uint32_t, InstrumentDisplay> instruments;
    QString refusal;
    QStringList skipped;                     ///< rows left out, with the reason
};

/// Build the provider-neutral snapshot the Terminal's positions and funds
/// tables accept. Refuses (with a reason) rather than truncating: more than
/// kMaxPositionsPerAccount open rows, or a missing identity.
[[nodiscard]] inline GetsTypedAccount gets_typed_account(const GetsAccount& a, const MasterIndex* master,
                                                         int reference_year) {
    using namespace broker_view;
    GetsTypedAccount out;
    if (!a.loaded) { out.refusal = a.error; return out; }
    const QByteArray id = a.account_id.toUtf8();
    if (id.isEmpty() || static_cast<std::size_t>(id.size()) > kBrokerAccountIdMax) {
        out.refusal = QStringLiteral("account id missing or too long");
        return out;
    }
    if (a.expires_at <= a.fetched_at) {
        out.refusal = QStringLiteral("snapshot has no validity window");
        return out;
    }
    std::uint64_t slot = 1469598103934665603ull;
    for (const char c : id) { slot ^= static_cast<std::uint8_t>(c); slot *= 1099511628211ull; }
    if (slot == 0) slot = 1;
    AccountSnapshot s{};
    s.account_session = SessionKey{BrokerId::Fyers, slot, static_cast<std::uint64_t>(a.fetched_at)};
    s.observed = EvidenceWindow{Timestamp{a.fetched_at * 1'000'000'000LL},
                                Timestamp{a.expires_at * 1'000'000'000LL}};
    for (qsizetype i = 0; i < id.size(); ++i) s.account_id[static_cast<std::size_t>(i)] = id[i];
    s.profile.status = SnapshotSectionStatus::Present;
    const auto section = [](int http, bool parsed) {
        SnapshotSection r{};
        r.http_status = static_cast<std::uint16_t>(http > 0 && http < 65536 ? http : 0);
        r.status = http <= 0 ? SnapshotSectionStatus::TransportError
                 : http != 200 ? SnapshotSectionStatus::HttpError
                 : parsed ? SnapshotSectionStatus::Present : SnapshotSectionStatus::Invalid;
        return r;
    };
    s.funds = section(a.funds_status, a.funds_ok);
    s.orders = section(a.orders_status, a.orders_ok);
    s.positions = section(a.positions_status, a.positions_ok);
    if (s.funds.status == SnapshotSectionStatus::Present) {
        const auto opt = [](std::optional<std::int64_t> v) {
            return v ? std::optional<Notional>{Notional{*v}} : std::nullopt;
        };
        s.typed_funds.cash = opt(gets_fund(a, 3));                        // clear balance
        s.typed_funds.available_trading_balance = opt(gets_fund(a, 10));  // available balance
        s.typed_funds.collateral = opt(gets_fund(a, 5));
        s.typed_funds.utilised_margin = opt(gets_fund(a, 2));
    }
    if (s.positions.status == SnapshotSectionStatus::Present) {
        s.typed_positions.account_session = s.account_session;
        s.typed_positions.observed = s.observed;
        std::size_t n = 0;
        for (const auto& p : a.positions) {
            if (p.net_qty == 0) continue;
            if (n == kMaxPositionsPerAccount) {
                out.refusal = QStringLiteral("more than %1 open positions; the typed table refuses rather than truncates")
                                  .arg(kMaxPositionsPerAccount);
                s.positions.status = SnapshotSectionStatus::Invalid;
                s.typed_positions.count = 0;
                break;
            }
            const auto key = gets_instrument_key(p.symbol);
            const auto product = gets_product(p.product);
            bool duplicate = false;
            for (std::size_t i = 0; i < n; ++i)
                duplicate = duplicate || (s.typed_positions.position[i].instrument == key
                                          && s.typed_positions.position[i].product == product);
            if (duplicate) {   // e.g. CO and BO both read as Intraday; the first row stands
                out.skipped << QStringLiteral("%1 %2: second row for the same product").arg(p.symbol, p.product);
                continue;
            }
            if (!p.net_avg || *p.net_avg < 0) {
                out.skipped << QStringLiteral("%1: no usable average price").arg(p.symbol);
                continue;
            }
            Position& row = s.typed_positions.position[n++];
            row.instrument = key;
            row.product = product;
            row.net_qty = Qty{p.net_qty};
            row.average_price = Price{*p.net_avg};
            if (p.ltp && *p.ltp > 0) row.last_mark = Price{*p.ltp};
            row.marked = s.observed;
            out.instruments.insert(static_cast<std::uint32_t>(key),
                                   gets_display(resolve_gets_instrument(p.symbol, master, reference_year)));
        }
        if (s.positions.status == SnapshotSectionStatus::Present) {
            s.typed_positions.count = static_cast<std::uint16_t>(n);
            s.typed_positions_present = true;
        }
    }
    out.snapshot = s;
    return out;
}

} // namespace altair::ui
