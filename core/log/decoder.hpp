#pragma once

// P0-07 — binary async logger: the off-thread decoder.
//
// This is where the formatting the hot path refused to do actually happens.
// It writes into a caller-supplied buffer and allocates nothing, so it is safe
// on a thread with its own budget. Every entry point truncates rather than
// overruns, and always NUL-terminates when capacity allows.
//
// Nothing here is ALTAIR_HOT — formatting is precisely what this design moves
// off the hot path.

#include <log/binlog.hpp>
#include <time/tsc_clock.hpp>
#include <types/units.hpp>

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>

namespace altair {

namespace detail {

/// Bounded text appender. Writes at most cap-1 bytes and always terminates.
/// `n` counts bytes ACTUALLY written, so it is the value to return.
class LogTextWriter {
public:
    constexpr LogTextWriter(char* out, std::size_t cap) noexcept
        : out_(out), cap_(cap) {}

    void put(char c) noexcept {
        if (n_ + 1 < cap_) {
            out_[n_] = c;
            ++n_;
        }
    }

    void put_str(const char* s) noexcept {
        if (s == nullptr) {
            return;
        }
        for (; *s != '\0'; ++s) {
            put(*s);
        }
    }

    void put_i64(std::int64_t v) noexcept {
        char tmp[24];
        const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
        for (char* q = tmp; q != res.ptr; ++q) {
            put(*q);
        }
    }

    void put_u64(std::uint64_t v) noexcept {
        char tmp[24];
        const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
        for (char* q = tmp; q != res.ptr; ++q) {
            put(*q);
        }
    }

    void put_f64(double v) noexcept {
        char tmp[40];
        const auto res =
            std::to_chars(tmp, tmp + sizeof(tmp), v, std::chars_format::general);
        for (char* q = tmp; q != res.ptr; ++q) {
            put(*q);
        }
    }

    /// Terminate and report bytes written, excluding the NUL.
    std::size_t finish() noexcept {
        if (cap_ > 0) {
            out_[n_] = '\0';
        }
        return n_;
    }

private:
    char* out_;
    std::size_t cap_;
    std::size_t n_ = 0;
};

/// Render one packed argument according to its tag.
inline void append_log_arg(LogTextWriter& w, const LogRecord& r,
                           std::size_t i) noexcept {
    switch (r.arg_tags[i]) {
        case ArgTag::I64:
            w.put_i64(std::bit_cast<std::int64_t>(r.args[i]));
            break;
        case ArgTag::U64:
            w.put_u64(r.args[i]);
            break;
        case ArgTag::F64:
            w.put_f64(std::bit_cast<double>(r.args[i]));
            break;
        case ArgTag::Bool:
            w.put_str(r.args[i] != 0 ? "true" : "false");
            break;
        case ArgTag::None:
        default:
            w.put_str("{none}");
            break;
    }
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// LogDecoder — turns records into text, OFF the producer thread.
// ─────────────────────────────────────────────────────────────────────────
class LogDecoder {
public:
    /// Borrow a registry and a clock. Both must outlive the decoder.
    /// The clock converts LogRecord::tsc_ticks to a UTC instant.
    LogDecoder(const LogRegistry& reg, const TscClock& clk) noexcept
        : reg_(&reg), clk_(&clk) {}

    /// Substitute the record's arguments into its site's format string.
    /// `{}` placeholders are filled positionally. A placeholder with no
    /// argument becomes `{?}`; arguments with no placeholder are appended
    /// as ` (+N unused)`. Format specs such as `{:.2f}` are NOT supported and
    /// are emitted literally.
    /// UNIT: returns bytes written, excluding the NUL.
    /// PRECONDITION: out has room for cap bytes. Returns 0 if cap == 0 or the
    /// site id is unknown.
    [[nodiscard]] std::size_t decode_message(const LogRecord& r,
                                             char* out,
                                             std::size_t cap) const noexcept {
        if (out == nullptr || cap == 0) {
            return 0;
        }
        out[0] = '\0';

        const LogSite* s = reg_->site(r.site_id);
        if (s == nullptr || s->fmt == nullptr) {
            return 0;
        }

        detail::LogTextWriter w{out, cap};
        std::size_t next = 0;

        for (const char* p = s->fmt; *p != '\0';) {
            if (p[0] == '{' && p[1] == '}') {
                if (next < r.arg_count) {
                    detail::append_log_arg(w, r, next);
                    ++next;
                } else {
                    w.put_str("{?}");
                }
                p += 2;
            } else {
                // A lone '{', or the start of a format spec, is literal. This
                // decoder deliberately does not interpret `{:.2f}`.
                w.put(*p);
                ++p;
            }
        }

        if (next < r.arg_count) {
            w.put_str(" (+");
            w.put_u64(static_cast<std::uint64_t>(r.arg_count - next));
            w.put_str(" unused)");
        }

        return w.finish();
    }

    /// A full line: `<ns> <LEVEL> <file>:<line> <message>`, where <ns> is the
    /// UTC instant in nanoseconds since the Unix epoch and <LEVEL> is a
    /// five-character left-aligned name. When dropped_before is non-zero the
    /// line is prefixed with `[dropped N] `.
    /// UNIT: returns bytes written, excluding the NUL.
    /// PRECONDITION: as decode_message.
    [[nodiscard]] std::size_t decode_line(const LogRecord& r,
                                          char* out,
                                          std::size_t cap) const noexcept {
        if (out == nullptr || cap == 0) {
            return 0;
        }
        out[0] = '\0';

        const LogSite* s = reg_->site(r.site_id);
        if (s == nullptr || s->fmt == nullptr) {
            return 0;
        }

        detail::LogTextWriter w{out, cap};

        if (r.dropped_before != 0) {
            w.put_str("[dropped ");
            w.put_u64(r.dropped_before);
            w.put_str("] ");
        }

        w.put_i64(instant_ns(r));
        w.put(' ');
        w.put_str(level_name(r.level));
        w.put(' ');
        w.put_str(s->file);
        w.put(':');
        w.put_u64(s->line);
        w.put(' ');

        // Substitute in place, continuing into the same writer.
        std::size_t next = 0;
        for (const char* p = s->fmt; *p != '\0';) {
            if (p[0] == '{' && p[1] == '}') {
                if (next < r.arg_count) {
                    detail::append_log_arg(w, r, next);
                    ++next;
                } else {
                    w.put_str("{?}");
                }
                p += 2;
            } else {
                w.put(*p);
                ++p;
            }
        }
        if (next < r.arg_count) {
            w.put_str(" (+");
            w.put_u64(static_cast<std::uint64_t>(r.arg_count - next));
            w.put_str(" unused)");
        }

        return w.finish();
    }

    /// Five-character left-aligned level name: "TRACE", "DEBUG", "INFO ",
    /// "WARN ", "ERROR". UNIT: none. Never returns nullptr.
    [[nodiscard]] static const char* level_name(LogLevel lvl) noexcept {
        switch (lvl) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Error: return "ERROR";
        }
        return "?????";
    }

private:
    /// Convert the record's raw ticks to ns since the Unix epoch.
    /// A record stamped before the clock's anchor would underflow the unsigned
    /// subtraction, so that case reports 0 rather than a nonsense instant.
    [[nodiscard]] std::int64_t instant_ns(const LogRecord& r) const noexcept {
        const TscCalibration& cal = clk_->calibration();
        if (r.tsc_ticks < cal.anchor_ticks) {
            return 0;
        }
        const Duration d = clk_->ticks_to_duration(r.tsc_ticks - cal.anchor_ticks);
        return (cal.anchor + d).ns_since_epoch();
    }

    const LogRegistry* reg_ = nullptr;
    const TscClock* clk_ = nullptr;
};

} // namespace altair
