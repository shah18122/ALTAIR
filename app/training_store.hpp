// app/training_store.hpp -- recorded FYERS sessions and dataset/ as ONE training input.
//
// Two jobs, both run nightly by ops/nightly.ps1 through altair_store:
//
// 1. DECODE a day's session tapes (altair_feed_recorder, live/tape.hpp) into
//    canonical files. The tape stays the lossless record; these are views:
//
//      data/store/ticks/<day>/bars_1m.csv   every instrument the feed carried
//      data/store/ticks/<day>/trades.csv    with --ticks: every print
//      data/store/ticks/<day>/quotes.csv    with --ticks: every market-watch update
//      data/store/ticks/<day>/books.csv     with --ticks: every book (5 or 50 levels)
//      data/store/ticks/<day>/_decode.json  what was read, dropped and missed
//      data/store/tape_bars/<seg>/<inst>/1m/<day>.csv   the indices and the
//                                           near futures, laid out like dataset/
//
//    SIM and replayed frames are dropped and counted: they are not the market.
//
// 2. SNAPSHOT: merge dataset/ (the owner's files), FYERS candles fetched each
//    night (data/store/fyers/, laid out like dataset/) and the bars built from
//    tapes into one tree, cut at ONE date, that every model trains on:
//
//      data/store/snapshots/<cutoff>/<seg>/<inst>/<tf>/*.csv
//      data/store/snapshots/<cutoff>/_snapshot.json    id, cutoff, rule, inputs, per-series counts
//      data/store/snapshots/<cutoff>/_conflicts.csv    every bar two sources disagreed on
//      data/store/snapshots/<cutoff>/_provenance.csv   which source each run of bars came from
//      data/store/snapshots/<cutoff>/_inputs/{bhavcopy,iv}/   the option inputs, same cutoff
//
// THE CANONICAL BAR. Time is the bar START in IST, written
// "YYYY-MM-DDTHH:MM:SS+05:30" (a daily bar: "YYYY-MM-DD"); stamps in any
// offset, or with seconds, are converted and floored by app/data_audit.hpp's
// loader, the one every model already reads through. Columns:
// time,open,high,low,close,volume,oi,source -- an absent volume or OI is an
// empty field, never 0. `source` names where the row came from; the loaders
// select columns by header name, so the extra column changes no reader.
//
// WHICH SOURCE WINS. Sources are given lowest priority first
// (`--source dataset=... --source fyers=...`), and `--fill tape=...` sources
// only ever fill a bar nobody else has. For a bar two sources both hold:
//   * an impossible bar (open or close outside high-low, or a price <= 0)
//     loses to a possible one, whichever its priority;
//   * within 1 bp on open, high, low and close (decimal places, the 1 bp rule
//     of app/dataset_merge.hpp) the LOWER-priority row stays, so a re-fetch
//     never churns history;
//   * beyond 1 bp the HIGHER-priority source wins, and the pair goes to
//     _conflicts.csv with both values.
// A row is never stitched from two sources.
//
// ONE CUTOFF. Every series, and the option inputs beside them, stop at the end
// of the same IST day. Nothing in a snapshot is dated after it, so a training
// run on it cannot see tomorrow; the curriculum's --leak-check then proves
// that within it no forecast saw its own future. A snapshot is built in a
// temp folder and renamed into place, and never rebuilt over: same cutoff,
// same inputs, same id.
#pragma once

#include <app/data_audit.hpp>
#include <app/price_text.hpp>
#include <live/tape.hpp>
#include <live/universe.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>
#include <server/quote_payload.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace altair::store {

namespace fs = std::filesystem;
namespace da = altair::data_audit;
namespace lv = altair::live;

inline constexpr std::int64_t kIstSec = 19'800;
inline constexpr double kAgreeBp = 1.0;

// ---- small text helpers ----------------------------------------------------------

/// Shortest round-trip text of a finite number; empty for NaN.
[[nodiscard]] inline std::string num(double v) {
    if (!std::isfinite(v)) return {};
    char b[64];
    const auto r = std::to_chars(b, b + sizeof b, v);
    return r.ec == std::errc{} ? std::string(b, r.ptr) : std::string{};
}

[[nodiscard]] inline std::string rupees(std::int64_t paise) {
    return num(static_cast<double>(paise) / 100.0);
}

/// "YYYY-MM-DDTHH:MM:SS.mmm+05:30" for UTC nanoseconds; empty for <= 0.
[[nodiscard]] inline std::string ist_ms_text(std::int64_t utc_ns) {
    if (utc_ns <= 0) return {};
    const std::int64_t ms = utc_ns / 1'000'000;
    const std::int64_t ist_s = ms / 1000 + kIstSec;
    std::string t = da::format_audit_time(ist_s, false);   // ...THH:MM:SS+05:30
    char frac[8];
    std::snprintf(frac, sizeof frac, ".%03lld", static_cast<long long>(ms % 1000));
    t.insert(19, frac);
    return t;
}

[[nodiscard]] inline std::string day_text(std::int64_t day) {
    return da::format_audit_time(day * da::kDaySec, true);
}

/// "YYYY-MM-DD" to days since 1970-01-01.
[[nodiscard]] inline std::optional<std::int64_t> parse_day(const std::string& s) {
    if (s.size() != 10) return std::nullopt;
    const auto t = da::parse_audit_time(s);
    if (!t) return std::nullopt;
    return da::audit_day(*t);
}

/// 64-bit FNV-1a, for input fingerprints (not security).
struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void add(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    }
    void add(const std::string& s) { add(s.data(), s.size()); }
    [[nodiscard]] std::string hex() const {
        char b[20];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
        return b;
    }
};

[[nodiscard]] inline std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

/// Write `bytes` to `path` through a temp and a rename. False on any failure.
[[nodiscard]] inline bool write_file(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (!f) { fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// ---- 1. decoding tapes -------------------------------------------------------------

/// Where an instrument's 1-minute bars go in a dataset-shaped tree, or "".
/// The indices and the NEAREST NSE future of NIFTY and BANKNIFTY (a
/// continuous near-month series, as dataset/fut/ holds); options and stocks
/// stay in bars_1m.csv only.
[[nodiscard]] inline std::string series_key(const lv::LiveInstrument& in, std::int64_t near_expiry) {
    if (in.kind == lv::LiveKind::Index) {
        if (in.fyers == "NSE:NIFTY50-INDEX") return "spot/nifty";
        if (in.fyers == "NSE:NIFTYBANK-INDEX") return "spot/banknifty";
        if (in.fyers == "NSE:INDIAVIX-INDEX") return "spot/indiavix";
        return {};
    }
    if (in.kind == lv::LiveKind::Future && in.fyers.rfind("NSE:", 0) == 0 && in.expiry_day == near_expiry
        && (in.underlying == "NIFTY" || in.underlying == "BANKNIFTY"))
        return in.underlying == "NIFTY" ? "fut/nifty" : "fut/banknifty";
    return {};
}

struct MinuteBar {
    std::int64_t t = 0;   ///< IST seconds, the minute's start
    std::int64_t o = 0, h = 0, l = 0, c = 0;   ///< paise
    std::int64_t vol_end = -1;                 ///< the day's cumulative volume at the last print
    std::int64_t oi = -1;
    std::int64_t trades = 0;
};

struct DecodeStats {
    std::size_t tapes = 0, tapes_skipped = 0, records = 0, frames = 0;
    std::size_t trades = 0, quotes = 0, books = 0, deep_books = 0;   ///< deep: more than 5 levels
    std::size_t snapshots = 0, heartbeats = 0, bad_frames = 0, framing_resets = 0;
    std::size_t simulated = 0, replayed = 0, unknown_token = 0, no_exch_ts = 0;
    std::size_t connects = 0, disconnects = 0, cut_bytes = 0;
    std::uint64_t missed[kTopicQuote + 1]{};   ///< frames the sequence says never arrived, by topic
    std::int64_t first_ns = 0, last_ns = 0;    ///< exchange time covered (UTC ns)
    std::vector<std::string> notes;
};

/// Reads tapes and keeps the minute bars; streams tick files when asked.
class TapeDecoder {
public:
    /// `ticks_dir` empty: bars only.
    explicit TapeDecoder(fs::path ticks_dir = {}) : dir_(std::move(ticks_dir)) {}

    /// One tape, start to end. False when it is not a session tape.
    bool read(const fs::path& tape) {
        lv::TapeReader in(tape.string());
        lv::TapeRecord rec;
        lv::TapeSections start;
        if (!in.ok() || !in.next(rec) || rec.kind != lv::TapeKind::Start || !lv::tape_unpack(rec.text(), start)) {
            ++st_.tapes_skipped;
            st_.notes.push_back(tape.filename().string() + ": not a session tape");
            return false;
        }
        ++st_.tapes;
        set_universe(start, tape);
        std::vector<std::uint8_t> buf;
        while (in.next(rec)) {
            ++st_.records;
            switch (rec.kind) {
            case lv::TapeKind::Start:   // a second session in one file: its own universe
                if (lv::tape_unpack(rec.text(), start)) set_universe(start, tape);
                buf.clear();
                break;
            case lv::TapeKind::Connected:
                ++st_.connects;
                buf.clear();
                for (auto& s : seq_) s = {};
                break;
            case lv::TapeKind::Disconnected: ++st_.disconnects; buf.clear(); break;
            case lv::TapeKind::Data: {
                buf.insert(buf.end(), rec.bytes.begin(), rec.bytes.end());
                std::size_t at = 0;
                while (buf.size() - at >= kFrameHeaderBytes) {
                    const auto h = decode_header(buf.data() + at, buf.size() - at);
                    if (!h) { ++st_.framing_resets; buf.clear(); at = 0; break; }
                    const std::size_t need = kFrameHeaderBytes + h->payload_len;
                    if (buf.size() - at < need) break;
                    on_frame(*h, buf.data() + at + kFrameHeaderBytes, rec.wall_ns);
                    at += need;
                }
                if (at > 0) buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            }
            default: break;   // the engine's control events: not market data
            }
        }
        st_.cut_bytes += buf.size();
        return true;
    }

    [[nodiscard]] const DecodeStats& stats() const noexcept { return st_; }

    /// Flush and close the tick files (--ticks). False when any write failed.
    bool close_ticks() {
        bool ok = true;
        for (auto* f : {&trades_, &quotes_, &books_}) {
            if (!*f) continue;
            (*f)->flush();
            ok = ok && static_cast<bool>(**f);
            f->reset();
        }
        return ok;
    }

    /// Every instrument's bars, by FYERS symbol.
    [[nodiscard]] const std::map<std::string, std::map<std::int64_t, MinuteBar>>& bars() const noexcept { return bars_; }
    [[nodiscard]] const std::map<std::string, lv::LiveInstrument>& instruments() const noexcept { return inst_; }
    [[nodiscard]] const std::map<std::string, std::string>& series() const noexcept { return series_; }
    /// The cumulative volume before each symbol's first bar (a snapshot, else
    /// the first print less its own quantity); -1 unknown.
    [[nodiscard]] std::int64_t volume_before(const std::string& sym) const {
        const auto it = vol_base_.find(sym);
        return it == vol_base_.end() ? -1 : it->second;
    }

    /// bars_1m.csv for every instrument.
    [[nodiscard]] std::string bars_csv() const {
        std::string o = "time,fyers,kind,underlying,expiry,strike,open,high,low,close,volume,oi,trades,source\n";
        for (const auto& [sym, mins] : bars_) {
            const auto& in = inst_.at(sym);
            std::int64_t prev = volume_before(sym);
            for (const auto& [t, b] : mins) {
                o += da::format_audit_time(b.t, false) + "," + sym + "," + lv::kind_text(in.kind) + "," + in.underlying
                     + "," + (in.expiry_day > 0 ? day_text(in.expiry_day) : std::string{}) + ","
                     + (in.strike > 0 ? num(in.strike) : std::string{}) + "," + rupees(b.o) + "," + rupees(b.h) + ","
                     + rupees(b.l) + "," + rupees(b.c) + "," + volume_text(b, prev) + ","
                     + (b.oi >= 0 ? std::to_string(b.oi) : std::string{}) + "," + std::to_string(b.trades) + ",fyers_tape\n";
            }
        }
        return o;
    }

    /// One dataset-shaped CSV per series ("spot/nifty" -> its bars).
    [[nodiscard]] std::map<std::string, std::string> series_csv() const {
        std::map<std::string, std::string> out;
        for (const auto& [sym, key] : series_) {
            const auto it = bars_.find(sym);
            if (it == bars_.end()) continue;
            std::string& o = out[key];
            if (o.empty()) o = "time,open,high,low,close,volume,oi,source\n";
            std::int64_t prev = volume_before(sym);
            const bool index = inst_.at(sym).kind == lv::LiveKind::Index;
            for (const auto& [t, b] : it->second) {
                const std::string vol = index ? std::string{} : volume_text(b, prev);
                o += da::format_audit_time(b.t, false) + "," + rupees(b.o) + "," + rupees(b.h) + "," + rupees(b.l) + ","
                     + rupees(b.c) + "," + vol + "," + (b.oi >= 0 && !index ? std::to_string(b.oi) : std::string{})
                     + ",fyers_tape\n";
            }
        }
        return out;
    }

    [[nodiscard]] std::string stats_json(const std::string& day) const {
        const auto& s = st_;
        std::string o = "{\n  \"day\": " + json_str(day) + ",\n  \"tapes\": " + std::to_string(s.tapes)
            + ",\n  \"tapes_skipped\": " + std::to_string(s.tapes_skipped) + ",\n  \"frames\": " + std::to_string(s.frames)
            + ",\n  \"trades\": " + std::to_string(s.trades) + ",\n  \"quotes\": " + std::to_string(s.quotes)
            + ",\n  \"books\": " + std::to_string(s.books) + ",\n  \"books_over_5_levels\": " + std::to_string(s.deep_books)
            + ",\n  \"snapshots\": " + std::to_string(s.snapshots) + ",\n  \"bad_frames\": " + std::to_string(s.bad_frames)
            + ",\n  \"framing_resets\": " + std::to_string(s.framing_resets)
            + ",\n  \"simulated_dropped\": " + std::to_string(s.simulated) + ",\n  \"replayed_dropped\": " + std::to_string(s.replayed)
            + ",\n  \"unknown_token\": " + std::to_string(s.unknown_token) + ",\n  \"no_exchange_time\": " + std::to_string(s.no_exch_ts)
            + ",\n  \"connects\": " + std::to_string(s.connects) + ",\n  \"disconnects\": " + std::to_string(s.disconnects)
            + ",\n  \"missed_trades\": " + std::to_string(s.missed[kTopicTrades]) + ",\n  \"missed_books\": " + std::to_string(s.missed[kTopicBook])
            + ",\n  \"missed_quotes\": " + std::to_string(s.missed[kTopicQuote]) + ",\n  \"bytes_cut_short\": " + std::to_string(s.cut_bytes)
            + ",\n  \"first_exchange_time\": " + json_str(ist_ms_text(s.first_ns)) + ",\n  \"last_exchange_time\": " + json_str(ist_ms_text(s.last_ns))
            + ",\n  \"instruments\": " + std::to_string(bars_.size()) + ",\n  \"notes\": [";
        for (std::size_t i = 0; i < s.notes.size(); ++i) o += (i ? ", " : "") + json_str(s.notes[i]);
        return o + "]\n}\n";
    }

private:
    [[nodiscard]] static std::string volume_text(const MinuteBar& b, std::int64_t& prev) {
        if (b.vol_end < 0) return {};
        const std::int64_t v = prev >= 0 && b.vol_end >= prev ? b.vol_end - prev : -1;
        prev = b.vol_end;
        return v >= 0 ? std::to_string(v) : std::string{};
    }

    void set_universe(const lv::TapeSections& start, const fs::path& tape) {
        by_token_.clear();
        const std::string* uni = lv::tape_section(start, "universe.csv");
        if (uni == nullptr || uni->empty()) {
            st_.notes.push_back(tape.filename().string() + ": no universe on the tape; its tokens cannot be named");
            return;
        }
        std::istringstream us(*uni);
        const auto u = lv::read_universe_stream(us);
        std::map<std::string, std::int64_t> near;   // underlying -> nearest NSE future expiry
        for (const auto& in : u)
            if (in.kind == lv::LiveKind::Future && in.fyers.rfind("NSE:", 0) == 0 && in.expiry_day > 0) {
                auto& n = near[in.underlying];
                if (n == 0 || in.expiry_day < n) n = in.expiry_day;
            }
        for (const auto& in : u) {
            if (in.fyers.empty()) continue;
            by_token_[in.token] = in.fyers;
            inst_.emplace(in.fyers, in);
            const auto it = near.find(in.underlying);
            const std::string key = series_key(in, it == near.end() ? 0 : it->second);
            if (!key.empty()) series_[in.fyers] = key;
        }
    }

    [[nodiscard]] const std::string* name(std::uint32_t token) {
        const auto it = by_token_.find(token);
        if (it == by_token_.end()) { ++st_.unknown_token; return nullptr; }
        return &it->second;
    }

    /// False for a frame the sequence says was already seen.
    bool sequence(const FrameHeader& h) {
        if (!h.advances_seq() || h.topic == 0 || h.topic > kTopicQuote) return true;
        auto& s = seq_[h.topic];
        if (h.kind == FrameKind::Snapshot || !s.primed) { s = {h.seq, true}; return true; }
        if (h.seq <= s.last) return false;
        if (h.seq > s.last + 1) st_.missed[h.topic] += h.seq - s.last - 1;
        s.last = h.seq;
        return true;
    }

    void open_ticks() {
        if (dir_.empty() || trades_) return;
        std::error_code ec;
        fs::create_directories(dir_, ec);
        trades_.emplace(dir_ / "trades.csv", std::ios::binary | std::ios::trunc);
        quotes_.emplace(dir_ / "quotes.csv", std::ios::binary | std::ios::trunc);
        books_.emplace(dir_ / "books.csv", std::ios::binary | std::ios::trunc);
        *trades_ << "recv_time,exch_time,fyers,last,last_qty,volume,oi,exch_time_is_recv,source\n";
        *quotes_ << "recv_time,fyers,open,high,low,prev_close,bid,ask,bid_qty,ask_qty,avg_price,total_buy,total_sell,"
                    "last_trade_time,source\n";
        *books_ << "recv_time,feed_time,fyers,levels,bids,asks,source\n";   // levels "price:qty:orders;..."
    }

    void on_frame(const FrameHeader& h, const std::uint8_t* body, std::int64_t wall_ns) {
        ++st_.frames;
        if (!h.advances_seq()) { ++st_.heartbeats; return; }
        if (h.topic < kTopicTrades || h.topic > kTopicQuote) return;
        if (!sequence(h)) return;
        open_ticks();
        if (h.topic == kTopicQuote) {
            const auto q = decode_quote(body, h.payload_len);
            if (!q) { ++st_.bad_frames; return; }
            if (q->has(kQuoteSimulated)) { ++st_.simulated; return; }
            if (q->has(kQuoteReplay)) { ++st_.replayed; return; }
            const std::string* sym = name(q->token);
            if (sym == nullptr) return;
            ++st_.quotes;
            if (quotes_) {
                const auto p = [](bool on, std::int64_t v) { return on ? rupees(v) : std::string{}; };
                const auto n = [](bool on, std::int64_t v) { return on ? std::to_string(v) : std::string{}; };
                *quotes_ << ist_ms_text(wall_ns) << ',' << *sym << ',' << p(q->has(kQuoteHasOhlc), q->open) << ','
                         << p(q->has(kQuoteHasOhlc), q->high) << ',' << p(q->has(kQuoteHasOhlc), q->low) << ','
                         << p(q->has(kQuoteHasPrevClose), q->prev_close) << ',' << p(q->has(kQuoteHasTop), q->bid) << ','
                         << p(q->has(kQuoteHasTop), q->ask) << ',' << n(q->has(kQuoteHasTop), q->bid_qty) << ','
                         << n(q->has(kQuoteHasTop), q->ask_qty) << ',' << p(q->has(kQuoteHasAtp), q->avg_price) << ','
                         << n(q->has(kQuoteHasTotals), q->total_buy) << ',' << n(q->has(kQuoteHasTotals), q->total_sell) << ','
                         << (q->has(kQuoteHasLtt) ? ist_ms_text(q->last_trade_ns) : std::string{}) << ",fyers_live\n";
            }
            return;
        }
        const auto d = decode_price(body, h.payload_len);
        if (!d) { ++st_.bad_frames; return; }
        const auto& p = d->payload;
        if (p.has(kPriceSimulated)) { ++st_.simulated; return; }
        if (p.has(kPriceReplay)) { ++st_.replayed; return; }
        const std::string* sym = name(p.token);
        if (sym == nullptr) return;
        if (h.topic == kTopicBook) {
            ++st_.books;
            if (p.depth_levels > 5) ++st_.deep_books;
            if (books_) {
                std::string bids, asks;
                for (std::uint16_t k = 0; k < p.depth_levels; ++k) {
                    if (k) { bids += ';'; asks += ';'; }
                    bids += rupees(d->bids[k].price_paise) + ":" + std::to_string(d->bids[k].qty) + ":" + std::to_string(d->bids[k].orders);
                    asks += rupees(d->asks[k].price_paise) + ":" + std::to_string(d->asks[k].qty) + ":" + std::to_string(d->asks[k].orders);
                }
                *books_ << ist_ms_text(wall_ns) << ',' << ist_ms_text(h.engine_time_ns) << ',' << *sym << ','
                        << p.depth_levels << ',' << bids << ',' << asks << ",fyers_live\n";
            }
            return;
        }
        // kTopicTrades. A snapshot is the state the stream starts from, not a print.
        if (h.kind == FrameKind::Snapshot) {
            ++st_.snapshots;
            if (p.has(kPriceHasVolume) && !vol_base_.contains(*sym)) vol_base_[*sym] = p.volume;
            return;
        }
        ++st_.trades;
        const bool recv_time = p.has(kPriceNoExchTs);
        if (recv_time) ++st_.no_exch_ts;
        const std::int64_t ns = p.exchange_ts_ns > 0 ? p.exchange_ts_ns : h.engine_time_ns;
        if (st_.first_ns == 0 || ns < st_.first_ns) st_.first_ns = ns;
        st_.last_ns = std::max(st_.last_ns, ns);
        if (trades_) {
            *trades_ << ist_ms_text(wall_ns) << ',' << ist_ms_text(ns) << ',' << *sym << ',' << rupees(p.last_paise) << ','
                     << p.last_qty << ',' << (p.has(kPriceHasVolume) ? std::to_string(p.volume) : std::string{}) << ','
                     << (p.has(kPriceHasOi) ? std::to_string(p.oi) : std::string{}) << ',' << (recv_time ? 1 : 0) << ",fyers_live\n";
        }
        if (p.last_paise <= 0) return;
        const std::int64_t ist_s = ns / 1'000'000'000 + kIstSec;
        const std::int64_t minute = ist_s - ((ist_s % 60) + 60) % 60;
        auto& mins = bars_[*sym];
        auto [it, fresh] = mins.try_emplace(minute);
        MinuteBar& b = it->second;
        if (fresh) {
            b.t = minute;
            b.o = b.h = b.l = p.last_paise;
            if (p.has(kPriceHasVolume) && !vol_base_.contains(*sym)) vol_base_[*sym] = std::max<std::int64_t>(0, p.volume - p.last_qty);
        }
        b.h = std::max(b.h, p.last_paise);
        b.l = std::min(b.l, p.last_paise);
        b.c = p.last_paise;   // the latest print of the minute (frames arrive in order)
        if (p.has(kPriceHasVolume)) b.vol_end = std::max(b.vol_end, p.volume);
        if (p.has(kPriceHasOi)) b.oi = p.oi;
        ++b.trades;
    }

    struct Seq { std::uint64_t last = 0; bool primed = false; };
    fs::path dir_;
    std::optional<std::ofstream> trades_, quotes_, books_;
    DecodeStats st_;
    Seq seq_[kTopicQuote + 1]{};
    std::unordered_map<std::uint32_t, std::string> by_token_;
    std::map<std::string, lv::LiveInstrument> inst_;
    std::map<std::string, std::string> series_;     ///< fyers -> "spot/nifty"
    std::map<std::string, std::int64_t> vol_base_;
    std::map<std::string, std::map<std::int64_t, MinuteBar>> bars_;
};

// ---- 2. the snapshot ---------------------------------------------------------------

struct Source {
    std::string name;
    fs::path dir;
    bool fill_only = false;
};

struct Conflict {
    std::string series, time, kept, dropped, why;
    double kept_close = 0, dropped_close = 0, bp = 0;
};

struct SeriesResult {
    std::string key;        ///< "spot/nifty/1m"
    std::size_t rows = 0, conflicts = 0, agreed = 0, invalid_dropped = 0, after_cutoff = 0, parse_errors = 0;
    std::map<std::string, std::size_t> by_source;
    std::int64_t first = 0, last = 0;   ///< IST seconds
    bool empty = true;
};

/// Impossible bars lose to possible ones.
[[nodiscard]] inline bool possible(const da::AuditBar& b) {
    return b.o > 0 && b.h > 0 && b.l > 0 && b.c > 0 && b.l <= b.h && b.o >= b.l && b.o <= b.h && b.c >= b.l && b.c <= b.h;
}

/// The largest open/high/low/close difference in basis points.
[[nodiscard]] inline double max_bp(const da::AuditBar& a, const da::AuditBar& b) {
    double m = 0.0;
    for (const auto& [x, y] : std::initializer_list<std::pair<double, double>>{{a.o, b.o}, {a.h, b.h}, {a.l, b.l}, {a.c, b.c}})
        if (x > 0 && y > 0) m = std::max(m, std::fabs(10'000.0 * std::log(y / x)));
    return m;
}

struct Picked {
    da::AuditBar bar;
    std::size_t source = 0;
};

/// Merge one series' sources (lowest priority first) by the rule in the header.
inline std::map<std::int64_t, Picked> merge_series(const std::vector<da::AuditSeries>& per_source,
                                                    const std::vector<Source>& sources, const std::string& key,
                                                    SeriesResult& r, std::vector<Conflict>& conflicts,
                                                    std::size_t max_conflicts) {
    std::map<std::int64_t, Picked> rows;
    const bool daily = key.size() > 3 && key.substr(key.size() - 3) == "/1d";
    for (std::size_t s = 0; s < per_source.size(); ++s) {
        r.parse_errors += per_source[s].parse_errors;
        for (const auto& b : per_source[s].bars) {
            auto it = rows.find(b.t);
            if (it == rows.end()) {
                if (!possible(b)) {
                    // Nothing better yet: keep it out, as the curriculum would widen it anyway --
                    // a later source may supply a possible bar for this time.
                    ++r.invalid_dropped;
                    continue;
                }
                rows.emplace(b.t, Picked{b, s});
                continue;
            }
            if (sources[s].fill_only) continue;
            Picked& cur = it->second;
            if (!possible(b)) { ++r.invalid_dropped; continue; }
            const double bp = max_bp(cur.bar, b);
            if (bp <= kAgreeBp) { ++r.agreed; continue; }
            ++r.conflicts;
            if (conflicts.size() < max_conflicts)
                conflicts.push_back({key, da::format_audit_time(b.t, daily), sources[s].name, sources[cur.source].name,
                                     "differs beyond 1 bp; higher priority wins", b.c, cur.bar.c, bp});
            cur = Picked{b, s};
        }
    }
    // Rows dropped as impossible that no source replaced: added back, flagged, so a gap is not invented.
    for (std::size_t s = 0; s < per_source.size(); ++s)
        for (const auto& b : per_source[s].bars)
            if (!possible(b) && !rows.contains(b.t) && b.c > 0) {
                rows.emplace(b.t, Picked{b, s});
                --r.invalid_dropped;
            }
    return rows;
}

struct SnapshotOptions {
    std::vector<Source> sources;      ///< lowest priority first; fill-only ones anywhere
    std::int64_t cutoff_day = 0;      ///< days since epoch: the last IST day included
    fs::path out;                     ///< the snapshot folder (must not exist)
    fs::path bhavcopy, iv;            ///< optional option inputs, cut at the same day
    std::size_t max_conflicts = 200'000;
};

struct SnapshotReport {
    std::vector<SeriesResult> series;
    std::size_t conflicts = 0, bhavcopy_files = 0, iv_files = 0;
    std::string id, error;
};

/// The date in an NSE F&O bhavcopy file name, or nullopt.
[[nodiscard]] inline std::optional<std::int64_t> bhavcopy_day(const std::string& name) {
    static const char* mon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    std::string up = name;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (up.rfind("FO", 0) == 0 && up.size() >= 11) {   // foDDMONYYYYbhav.csv
        for (int m = 0; m < 12; ++m)
            if (up.compare(4, 3, mon[m]) == 0) {
                const std::string s = up.substr(7, 4) + "-" + (m < 9 ? "0" : "") + std::to_string(m + 1) + "-" + up.substr(2, 2);
                return parse_day(s);
            }
    }
    const auto at = up.find("_FO_0_0_0_");
    if (at != std::string::npos && up.size() >= at + 18) {   // BhavCopy_NSE_FO_0_0_0_YYYYMMDD_F_0000.csv
        const std::string d = up.substr(at + 10, 8);
        return parse_day(d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2));
    }
    return std::nullopt;
}

/// Every <seg>/<inst>/<tf> under the sources, in order.
[[nodiscard]] inline std::vector<std::string> discover(const std::vector<Source>& sources) {
    std::set<std::string> keys;
    std::error_code ec;
    for (const auto& src : sources) {
        for (const auto& seg : fs::directory_iterator(src.dir, ec)) {
            const std::string sn = seg.path().filename().string();
            if (!seg.is_directory() || sn.empty() || sn[0] == '_' || sn[0] == '.') continue;
            for (const auto& inst : fs::directory_iterator(seg.path(), ec)) {
                const std::string in = inst.path().filename().string();
                if (!inst.is_directory() || in.empty() || in[0] == '_' || in[0] == '.') continue;
                for (const auto& tf : fs::directory_iterator(inst.path(), ec))
                    if (tf.is_directory() && da::audit_tf_from_name(tf.path().filename().string()))
                        keys.insert(sn + "/" + in + "/" + tf.path().filename().string());
            }
        }
    }
    return {keys.begin(), keys.end()};
}

/// Fingerprint of the files a series reads from one source (names, sizes, bytes).
inline void fingerprint_dir(const fs::path& dir, Fnv& f, std::size_t& files) {
    std::vector<fs::path> list;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (e.is_regular_file() && !n.empty() && n[0] != '.' && n[0] != '_' && e.path().extension() == ".csv") list.push_back(e.path());
    }
    std::sort(list.begin(), list.end());
    std::vector<char> buf(1 << 16);
    for (const auto& p : list) {
        f.add(p.filename().string());
        std::ifstream in(p, std::ios::binary);
        while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
            f.add(buf.data(), static_cast<std::size_t>(in.gcount()));
            if (!in) break;
        }
        ++files;
    }
}

[[nodiscard]] inline std::string bar_row(const da::AuditBar& b, bool daily, const std::string& source) {
    const auto px = [](double v) {
        char buf[altair::dataset::kPriceTextMax];
        const auto n = altair::dataset::format_price(v, buf, sizeof buf);
        return n ? std::string(buf, *n) : num(v);
    };
    return da::format_audit_time(b.t, daily) + "," + px(b.o) + "," + px(b.h) + "," + px(b.l) + "," + px(b.c) + ","
           + num(b.v) + "," + num(b.oi) + "," + source + "\n";
}

/// Build the snapshot. On any failure nothing is left at `o.out`.
[[nodiscard]] inline SnapshotReport build_snapshot(const SnapshotOptions& o) {
    SnapshotReport rep;
    std::error_code ec;
    if (fs::exists(o.out, ec)) { rep.error = o.out.string() + " exists: a snapshot is never rebuilt over"; return rep; }
    if (o.sources.empty()) { rep.error = "no sources"; return rep; }
    const fs::path tmp = o.out.string() + ".building";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) { rep.error = "cannot create " + tmp.string(); return rep; }
    const std::int64_t end = (o.cutoff_day + 1) * da::kDaySec;   // IST seconds: first instant excluded

    Fnv id;
    id.add("cutoff=" + day_text(o.cutoff_day) + ";rule=v1;");
    std::vector<Conflict> conflicts;
    std::string provenance = "series,from,to,source,rows\n";
    std::string inputs_json;
    for (const auto& key : discover(o.sources)) {
        const int tf = *da::audit_tf_from_name(key.substr(key.rfind('/') + 1));
        const bool daily = tf == da::kDailyTf;
        SeriesResult r;
        r.key = key;
        std::vector<da::AuditSeries> per;
        for (const auto& src : o.sources) {
            const fs::path d = src.dir / key;
            per.push_back(fs::is_directory(d, ec) ? da::load_audit_dir(d, tf) : da::AuditSeries{});
            std::size_t files = 0;
            Fnv f;
            if (fs::is_directory(d, ec)) fingerprint_dir(d, f, files);
            id.add(key + "|" + src.name + "|" + f.hex() + ";");
            if (files > 0)
                inputs_json += std::string(inputs_json.empty() ? "" : ",\n") + "    {\"series\": " + json_str(key)
                               + ", \"source\": " + json_str(src.name) + ", \"files\": " + std::to_string(files)
                               + ", \"fnv1a64\": " + json_str(f.hex()) + "}";
        }
        auto rows = merge_series(per, o.sources, key, r, conflicts, o.max_conflicts);
        // The cutoff: nothing after the end of the cutoff day.
        for (auto it = rows.lower_bound(end); it != rows.end();) { ++r.after_cutoff; it = rows.erase(it); }
        std::map<std::string, std::string> files;   // file name -> bytes
        std::string run_src;
        std::int64_t run_from = 0, run_to = 0;
        std::size_t run_n = 0;
        const auto flush_run = [&] {
            if (run_n > 0)
                provenance += key + "," + da::format_audit_time(run_from, daily) + "," + da::format_audit_time(run_to, daily) + ","
                              + run_src + "," + std::to_string(run_n) + "\n";
            run_n = 0;
        };
        for (const auto& [t, p] : rows) {
            const std::string& src = o.sources[p.source].name;
            const std::string month = da::format_audit_time(t, true).substr(0, 7);
            std::string& bytes = files[daily ? std::string("all.csv") : month + ".csv"];
            if (bytes.empty()) bytes = "time,open,high,low,close,volume,oi,source\n";
            bytes += bar_row(p.bar, daily, src);
            ++r.rows;
            ++r.by_source[src];
            if (r.empty) { r.first = t; r.empty = false; }
            r.last = t;
            if (run_n > 0 && src != run_src) flush_run();
            if (run_n == 0) { run_src = src; run_from = t; }
            run_to = t;
            ++run_n;
        }
        flush_run();
        for (const auto& [name, bytes] : files)
            if (!write_file(tmp / key / name, bytes)) {
                rep.error = "cannot write " + (tmp / key / name).string();
                fs::remove_all(tmp, ec);
                return rep;
            }
        rep.conflicts += r.conflicts;
        rep.series.push_back(std::move(r));
    }

    // The option inputs, cut at the same day.
    const auto link_or_copy = [&](const fs::path& from, const fs::path& to) {
        std::error_code e;
        fs::create_directories(to.parent_path(), e);
        fs::create_hard_link(from, to, e);
        if (e) { e.clear(); fs::copy_file(from, to, fs::copy_options::overwrite_existing, e); }
        return !e;
    };
    if (!o.bhavcopy.empty() && fs::is_directory(o.bhavcopy, ec)) {
        Fnv f;
        for (auto it = fs::recursive_directory_iterator(o.bhavcopy, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const std::string n = it->path().filename().string();
            const auto d = bhavcopy_day(n);
            if (!d || *d > o.cutoff_day) continue;
            if (link_or_copy(it->path(), tmp / "_inputs" / "bhavcopy" / n)) {
                ++rep.bhavcopy_files;
                f.add(n + ":" + std::to_string(it->file_size()) + ";");
            }
        }
        id.add("bhavcopy|" + f.hex() + ";");
    }
    if (!o.iv.empty() && fs::is_directory(o.iv, ec)) {
        Fnv f;
        for (const auto& sym : fs::directory_iterator(o.iv, ec)) {
            if (!sym.is_directory()) continue;
            for (const auto& e : fs::directory_iterator(sym.path(), ec)) {
                const std::string n = e.path().filename().string();
                const auto d = n.size() == 14 ? parse_day(n.substr(0, 10)) : std::nullopt;   // YYYY-MM-DD.csv
                if (!d || *d > o.cutoff_day || e.path().extension() != ".csv") continue;
                if (link_or_copy(e.path(), tmp / "_inputs" / "iv" / sym.path().filename() / n)) {
                    ++rep.iv_files;
                    f.add(sym.path().filename().string() + "/" + n + ":" + std::to_string(e.file_size()) + ";");
                }
            }
        }
        id.add("iv|" + f.hex() + ";");
    }

    std::string cx = "series,time,kept_source,kept_close,dropped_source,dropped_close,max_bp,why\n";
    for (const auto& c : conflicts)
        cx += c.series + "," + c.time + "," + c.kept + "," + num(c.kept_close) + "," + c.dropped + "," + num(c.dropped_close) + ","
              + num(std::round(c.bp * 100.0) / 100.0) + "," + c.why + "\n";
    rep.id = id.hex();
    std::string j = "{\n  \"id\": " + json_str(rep.id) + ",\n  \"cutoff\": " + json_str(day_text(o.cutoff_day))
        + ",\n  \"rule\": \"v1: sources lowest priority first; impossible bars lose; within 1 bp the lower-priority row "
          "stays; beyond 1 bp the higher-priority source wins and is listed in _conflicts.csv; fill-only sources only add "
          "missing bars; nothing after the cutoff day (IST)\",\n  \"sources\": [";
    for (std::size_t i = 0; i < o.sources.size(); ++i)
        j += std::string(i ? ", " : "") + "{\"name\": " + json_str(o.sources[i].name) + ", \"dir\": "
             + json_str(fs::absolute(o.sources[i].dir, ec).generic_string()) + ", \"fill_only\": " + (o.sources[i].fill_only ? "true" : "false") + "}";
    j += "],\n  \"conflicts\": " + std::to_string(rep.conflicts) + ",\n  \"conflicts_listed\": " + std::to_string(conflicts.size())
         + ",\n  \"bhavcopy_files\": " + std::to_string(rep.bhavcopy_files) + ",\n  \"iv_files\": " + std::to_string(rep.iv_files)
         + ",\n  \"series\": [";
    for (std::size_t i = 0; i < rep.series.size(); ++i) {
        const auto& r = rep.series[i];
        const bool daily = r.key.size() > 3 && r.key.substr(r.key.size() - 3) == "/1d";
        j += std::string(i ? "," : "") + "\n    {\"key\": " + json_str(r.key) + ", \"rows\": " + std::to_string(r.rows)
             + ", \"first\": " + json_str(r.empty ? "" : da::format_audit_time(r.first, daily))
             + ", \"last\": " + json_str(r.empty ? "" : da::format_audit_time(r.last, daily))
             + ", \"days_behind_cutoff\": " + std::to_string(r.empty ? -1 : o.cutoff_day - da::audit_day(r.last))
             + ", \"agreed\": " + std::to_string(r.agreed) + ", \"conflicts\": " + std::to_string(r.conflicts)
             + ", \"impossible_dropped\": " + std::to_string(r.invalid_dropped) + ", \"after_cutoff\": " + std::to_string(r.after_cutoff)
             + ", \"parse_errors\": " + std::to_string(r.parse_errors) + ", \"by_source\": {";
        std::size_t k = 0;
        for (const auto& [s, n] : r.by_source) j += std::string(k++ ? ", " : "") + json_str(s) + ": " + std::to_string(n);
        j += "}}";
    }
    j += "\n  ],\n  \"inputs\": [\n" + inputs_json + "\n  ]\n}\n";
    if (!write_file(tmp / "_conflicts.csv", cx) || !write_file(tmp / "_provenance.csv", provenance)
        || !write_file(tmp / "_snapshot.json", j)) {
        rep.error = "cannot write the snapshot's reports";
        fs::remove_all(tmp, ec);
        return rep;
    }
    fs::rename(tmp, o.out, ec);
    if (ec) { rep.error = "cannot rename " + tmp.string() + " to " + o.out.string() + ": " + ec.message(); fs::remove_all(tmp, ec); }
    return rep;
}

} // namespace altair::store
