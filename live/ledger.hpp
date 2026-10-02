// live/ledger.hpp -- the paper ledger on disk: what the engine did, written
// once, in order, and never half-written.
//
// The journal (every fill: the record the book is rebuilt from) comes first,
// then the views of it -- trades.csv, fills.csv, decisions.csv, margin.csv --
// then the open-positions snapshot, replaced atomically. Each CSV is a
// LiveCsvLog: rows queue in memory and are appended all-or-nothing. A write
// that fails (a full disk, a directory gone, a file made read-only) keeps
// every row queued and cuts the file back to its last good length, so the
// retry writes each row exactly once; a torn half-line can never be followed
// by a duplicate. The CLI halts new entries while anything is unwritten
// (halt_text) and lifts the halt on the first flush that lands everything.

#pragma once

#include <live/engine.hpp>
#include <live/paper.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace altair::live {

/// "2026-09-24 09:20:00" (IST) for a feed time in ns; empty for none.
[[nodiscard]] inline std::string live_ist_stamp(std::int64_t ns) {
    if (ns <= 0) return "";
    const std::int64_t s = ns / 1'000'000'000LL + 19800;
    const std::int64_t day = s / 86400, sec = s % 86400;
    char b[48];
    std::snprintf(b, sizeof b, "%s %02lld:%02lld:%02lld", day_text(day).c_str(), static_cast<long long>(sec / 3600),
                  static_cast<long long>(sec / 60 % 60), static_cast<long long>(sec % 60));
    return b;
}

/// One CSV, appended all-or-nothing.
class LiveCsvLog {
public:
    LiveCsvLog(std::filesystem::path path, std::string header) : path_(std::move(path)), header_(std::move(header)) {}

    void add(std::string row) { rows_.push_back(std::move(row)); }
    [[nodiscard]] std::size_t pending() const noexcept { return rows_.size(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Append every queued row (and the header, to an empty file). True when
    /// all of it reached the file; false leaves every row queued and the file
    /// as it was before the attempt.
    bool flush() {
        if (rows_.empty()) return true;
        namespace fs = std::filesystem;
        std::error_code ec;
        std::uintmax_t size = 0;
        if (fs::exists(path_, ec)) {
            if (!fs::is_regular_file(path_, ec)) return false;
            size = fs::file_size(path_, ec);
            if (ec) return false;
        }
        // The single writer (live/file_lock.hpp) knows the file's good length:
        // anything past it is a failed attempt's torn tail -- cut it first.
        if (good_ && size > *good_) {
            fs::resize_file(path_, *good_, ec);
            if (ec) return false;
            size = *good_;
        }
        std::string body;
        if (size == 0) body += header_ + '\n';
        for (const auto& r : rows_) body += r + '\n';
        bool ok = false;
        {
            std::ofstream f(path_, std::ios::binary | std::ios::app);
            if (f) {
                f.write(body.data(), static_cast<std::streamsize>(body.size()));
                f.flush();
                ok = static_cast<bool>(f);
            }
        }
        if (!ok) {
            if (fs::exists(path_, ec) && fs::is_regular_file(path_, ec)) {
                fs::resize_file(path_, size, ec);   // best effort now; the next flush cuts it if this did not
            }
            good_ = size;
            return false;
        }
        good_ = size + body.size();
        rows_.clear();
        return true;
    }

private:
    std::filesystem::path path_;
    std::string header_;
    std::vector<std::string> rows_;
    std::optional<std::uintmax_t> good_;   ///< the file's length after our last write, once known
};

inline constexpr const char* kLiveTradesHeader =
    "date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs";
inline constexpr const char* kLiveFillsHeader = "time,model,symbol,token,side,qty,price,expenses,at_quote,reason,source,costs";
inline constexpr const char* kLiveDecisionsHeader = "time,ns,model,decision";
inline constexpr const char* kLiveMarginHeader = "time,ns,margin_estimate,positions";

/// A CSV text field: quoted, quotes doubled, line breaks made spaces.
[[nodiscard]] inline std::string live_csv_text(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"') o += "\"\"";
        else if (c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    return o + "\"";
}

/// The paper ledger in one directory (data/live/paper, or a replay's).
class LivePaperLedger {
public:
    LivePaperLedger(const std::filesystem::path& dir, std::string costs_label)
        : journal(dir / "journal.csv", kLiveJournalHeader),
          trades(dir / "trades.csv", kLiveTradesHeader),
          fills(dir / "fills.csv", kLiveFillsHeader),
          decisions(dir / "decisions.csv", kLiveDecisionsHeader),
          margin(dir / "margin.csv", kLiveMarginHeader),
          positions_path_(dir / "open_positions.csv"),
          costs_(std::move(costs_label)) {}

    LiveCsvLog journal, trades, fills, decisions, margin;

    /// Queue what the engine produced since the last call. Returns the fills
    /// taken (the CLI times them).
    std::vector<LivePaperFill> collect(LiveEngine& e) {
        const std::string src = e.simulated() ? "SIM" : "LIVE";
        auto new_fills = e.take_new_fills();
        for (const auto& f : new_fills) {
            journal.add(live_journal_row(f));
            fills.add(live_ist_stamp(f.ns) + ",\"" + f.model + "\"," + f.symbol + "," + std::to_string(f.token) + ","
                      + (f.side > 0 ? "buy" : "sell") + "," + std::to_string(f.qty) + "," + live_fmt::num(f.price) + ","
                      + (std::isfinite(f.expenses) ? live_fmt::num(f.expenses) : "") + "," + (f.at_quote ? "1" : "0") + ",\""
                      + f.reason + "\"," + src + "," + costs_);
        }
        const auto q = [](const std::string& s) { return "\"" + s + "\""; };
        for (const auto& t : e.take_new_trades()) {
            trades.add(day_text(live_day_of(live_ist_minute_index(t.exit_ns))) + "," + q(t.model) + "," + t.symbol + ","
                       + std::to_string(t.token) + "," + (t.side > 0 ? "long" : "short") + "," + std::to_string(t.qty) + ","
                       + live_ist_stamp(t.entry_ns) + "," + live_fmt::num(t.entry) + "," + live_ist_stamp(t.exit_ns) + ","
                       + live_fmt::num(t.exit) + "," + live_fmt::num(t.gross) + ","
                       + (std::isfinite(t.expenses) ? live_fmt::num(t.expenses) : "") + ","
                       + (std::isfinite(t.net) ? live_fmt::num(t.net) : "") + "," + q(t.why_in) + "," + q(t.why_out) + ","
                       + src + "," + costs_);
        }
        for (const auto& d : e.take_decisions())
            decisions.add(live_ist_stamp(d.ns) + "," + std::to_string(d.ns) + "," + live_csv_text(d.model) + "," + live_csv_text(d.text));
        for (const auto& m : e.take_margin_samples())
            margin.add(live_ist_stamp(m.ns) + "," + std::to_string(m.ns) + "," + (std::isfinite(m.margin) ? live_fmt::num(m.margin, 0) : "")
                       + "," + std::to_string(m.positions));
        positions_dirty_ = e.take_positions_changed() || positions_dirty_;
        return new_fills;
    }

    /// Write everything queued: the journal first, then the views, then the
    /// positions snapshot. Empty when all of it landed; otherwise the first
    /// path that did not, and what failed stays queued. The views wait while
    /// the journal cannot be written: none may show a fill the record lacks.
    std::string flush(const LiveEngine& e) {
        if (!journal.flush()) return journal.path().string();
        std::string failed;
        for (LiveCsvLog* view : {&trades, &fills, &decisions, &margin})
            if (!view->flush() && failed.empty()) failed = view->path().string();
        if (positions_dirty_) {
            if (live_write_positions(positions_path_.string(), e.book().held())) positions_dirty_ = false;
            else if (failed.empty()) failed = positions_path_.string();
        }
        return failed;
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        return journal.pending() + trades.pending() + fills.pending() + decisions.pending() + margin.pending()
             + (positions_dirty_ ? 1u : 0u);
    }

    /// The engine's halt while `failed` is unwritten; empty when nothing is.
    [[nodiscard]] static std::string halt_text(const std::string& failed) {
        return failed.empty() ? std::string() : "cannot write " + failed + " (rows kept; retrying)";
    }

private:
    std::filesystem::path positions_path_;
    std::string costs_;
    bool positions_dirty_ = true;
};

} // namespace altair::live
