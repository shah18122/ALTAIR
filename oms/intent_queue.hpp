// oms/intent_queue.hpp -- draining the intent queue without trusting the file.
//
// CX02-B2. Fixes C13-008, and the queue half of C13-009 (intents never
// expired). CX02-B2b answers the independent review (review/REVIEW_AB.md,
// R-AB-007 … R-AB-014).
//
// P25-03's `drain_intents(path, offset)` trusted three things it could not
// know:
//
//   * That a file it could not open was an EMPTY queue. A locked, missing or
//     unreadable file returned zero intents and no error -- the same answer as
//     "nothing new", so an operator's request could sit unread with nothing
//     saying the reader was blind.
//   * That the file behind the offset was the file the offset was taken in.
//     Truncated, replaced or hand-edited, it silently skipped or resumed
//     mid-record.
//   * That an intent was still wanted however old it was. Nothing expired, so
//     a drainer started on Thursday would have acted on Monday's request.
//
// And it had no bound on batch size or line length, and no dedupe: a crash
// after acting but before saving the offset re-drained the same requests.
//
// WHAT THIS DOES.
//
//   * Unreadable is an ERROR. Truncated (file shorter than the cursor) and
//     Replaced (the bytes just before the cursor are not the ones it was taken
//     over) are errors. None of them advances the cursor.
//   * Each accepted intent is younger than `ttl_ns` and not dated further in
//     the future than the tolerated clock skew. The rest are quarantined with
//     the reason, and counted PER KIND as well as in total, so a capped list
//     never hides that duplicates occurred (R-AB-007).
//   * Ids are deduplicated across the TTL window, and the remembered ids are
//     SAVED WITH THE CURSOR.
//   * Invalid lines are QUARANTINED and counted, and the valid lines around
//     them continue. Decided (prompts/cx02/README.md, B2): a torn tail is the
//     expected residue of a crashed writer, and jamming the queue behind it
//     would turn one crash into a standing outage. Every accepted intent still
//     passes the mandatory risk check before anything is sent.
//   * Every bound refuses or reports (rule 11): at most `max_batch` accepted
//     and `max_lines` read per call (`more` says there is more); a line longer
//     than kMaxIntentLineBytes is consumed without being stored and
//     quarantined as TooLong; the quarantine LIST is capped but its TOTAL and
//     its per-kind counts are not; a full dedupe table refuses new ids rather
//     than forgetting live ones. A policy with a zero bound or a negative
//     duration is refused at construction (`create`), because `max_batch = 0`
//     would consume nothing for ever (R-AB-010).
//
// HOW A CALLER USES IT, AND WHY THE ORDER MATTERS.
//
//     auto batch = drainer.drain(path, cursor, now_ns);       // 1
//     if (!save_drainer_state(state_path, batch->next, drainer)) {
//         drainer.rollback(*batch);                           // 2
//         // nothing was acted on; the same lines drain again next time
//     } else {
//         act_on(batch->accepted);                            // 3
//     }
//
// The ids in a batch are remembered as soon as `drain` returns, so a second
// drain cannot hand the same request out twice. If the SAVE then fails, the
// caller must `rollback` -- otherwise requests that were never acted on come
// back as DuplicateId and are never executed (R-AB-008).
//
// This queue is AT-MOST-ONCE by design. A crash between the save and the act
// loses that batch rather than repeating it: an order placed twice is worse
// than an order not placed, and the UI's PENDING line is still on screen.
//
// SINGLE DRAINER. One process drains one queue file; the state file's temp
// name is fixed and assumes that. Durability is the OS's: there is no fsync,
// so a power loss can leave a short state file, which is then CorruptState and
// refuses to start rather than starting over (R-AB-013).
//
// The cursor, the dedupe table and the file are the drainer's alone. Nothing
// here is shared across threads.

#pragma once

#include <oms/order_intent.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace altair::oms {

inline constexpr std::uint64_t kIntentFnvBasis = 14'695'981'039'346'656'037ULL;
inline constexpr std::uint64_t kIntentFnvPrime = 1'099'511'628'211ULL;
/// How many bytes before the cursor identify the file it was taken in.
inline constexpr std::size_t kCursorTailBytes = 256;

/// Where to resume, and a fingerprint of what came before it.
struct IntentCursor {
    std::uint64_t offset = 0;
    /// FNV-1a over the (up to) kCursorTailBytes bytes ending at `offset`.
    std::uint64_t tail_fnv = kIntentFnvBasis;
};

enum class QueueError : std::uint8_t {
    /// The queue file could not be opened or read. NOT an empty queue.
    Unreadable,
    /// The file is shorter than the cursor.
    Truncated,
    /// The bytes before the cursor are not the ones it was taken over.
    Replaced,
    /// No saved drainer state exists at the path given. Only ever returned
    /// when the file is genuinely absent -- a stat that FAILED is Unreadable
    /// (R-AB-009), because starting over would re-accept in-TTL requests.
    NoState,
    /// Saved state exists and does not parse. Not treated as "no state":
    /// starting over from offset 0 without the ids would re-accept requests.
    CorruptState,
    /// Saved state could not be written.
    StateWriteFailed,
    /// A DrainPolicy with a zero bound or a negative duration.
    BadPolicy,
    /// `now_ns` is outside the parser's supported timestamp range.
    BadClock
};

enum class QuarantineKind : std::uint8_t {
    Unparseable,
    TooLong,
    Expired,
    FutureDated,
    DuplicateId,
    /// The dedupe table was full of live ids. Refused rather than accepted
    /// unchecked -- fail closed.
    DedupeFull
};

struct Quarantined {
    /// Byte offset of the start of the line.
    std::uint64_t offset = 0;
    QuarantineKind kind = QuarantineKind::Unparseable;
    /// Meaningful for Unparseable and TooLong.
    IntentError parse_error = IntentError::Malformed;
};

struct DrainPolicy {
    /// An intent older than this at drain time is expired. 30 s: a request is
    /// a person's decision about a price they were looking at.
    std::int64_t ttl_ns = 30'000'000'000LL;
    /// How far in the future `at` may be before it is refused as mis-stamped.
    std::int64_t max_future_ns = 5'000'000'000LL;
    std::size_t max_batch = 64;
    std::size_t max_lines = 4096;
    std::size_t max_quarantine_list = 64;
    std::size_t dedupe_capacity = 4096;
};

/// Refusals by reason. The LIST of quarantined lines is capped; these are not
/// (R-AB-007), so "1,200 expired and 2 duplicates" stays legible after an
/// outage.
struct RefusalCounts {
    std::size_t unparseable = 0;
    std::size_t too_long = 0;
    std::size_t expired = 0;
    std::size_t future_dated = 0;
    std::size_t duplicate_ids = 0;
    std::size_t dedupe_full = 0;

    [[nodiscard]] std::size_t total() const noexcept {
        return unparseable + too_long + expired + future_dated + duplicate_ids
             + dedupe_full;
    }
};

struct IntentBatch {
    std::vector<OrderIntent> accepted;
    /// The first `max_quarantine_list` refusals, with their offsets.
    std::vector<Quarantined> quarantined;
    /// Every refusal, by reason. `refused.total()` counts them all.
    RefusalCounts refused;
    /// Complete lines examined, blank ones included.
    std::size_t lines = 0;
    /// Resume here. Always at a line boundary.
    IntentCursor next;
    /// Stopped at max_batch or max_lines with complete lines unread.
    bool more = false;
    /// The file ends in an unterminated line, left for the next drain.
    bool partial_tail = false;
};

namespace detail {

[[nodiscard]] inline std::uint64_t fnv1a(const char* p, std::size_t n) noexcept {
    std::uint64_t h = kIntentFnvBasis;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= kIntentFnvPrime;
    }
    return h;
}

/// The last kCursorTailBytes bytes seen, oldest first when read out.
class TailRing {
public:
    void push(char c) noexcept {
        buf_[(head_ + len_) % kCursorTailBytes] = c;
        if (len_ < kCursorTailBytes) {
            ++len_;
        } else {
            head_ = (head_ + 1) % kCursorTailBytes;
        }
    }
    [[nodiscard]] std::uint64_t hash() const noexcept {
        std::uint64_t h = kIntentFnvBasis;
        for (std::size_t i = 0; i < len_; ++i) {
            h ^= static_cast<unsigned char>(buf_[(head_ + i) % kCursorTailBytes]);
            h *= kIntentFnvPrime;
        }
        return h;
    }

private:
    std::array<char, kCursorTailBytes> buf_{};
    std::size_t head_ = 0;
    std::size_t len_ = 0;
};

} // namespace detail

class IntentDrainer {
public:
    /// The only way to build one: a policy with a zero bound or a negative
    /// duration is REFUSED rather than accepted and livelocked (R-AB-010).
    [[nodiscard]] static std::expected<IntentDrainer, QueueError>
    create(DrainPolicy p = {}) {
        if (p.ttl_ns <= 0 || p.max_future_ns < 0 || p.max_batch == 0
            || p.max_lines == 0 || p.max_quarantine_list == 0
            || p.dedupe_capacity == 0) {
            return std::unexpected(QueueError::BadPolicy);
        }
        return IntentDrainer{p};
    }

    /// Read complete lines from `from` onward. `now_ns` is the drainer's
    /// clock: intents are aged against it.
    [[nodiscard]] std::expected<IntentBatch, QueueError>
    drain(const std::string& path, IntentCursor from, std::int64_t now_ns) {
        if (!detail::valid_intent_timestamp(now_ns)) {
            return std::unexpected(QueueError::BadClock);
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) { return std::unexpected(QueueError::Unreadable); }
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        if (size < 0) { return std::unexpected(QueueError::Unreadable); }
        if (static_cast<std::uint64_t>(size) < from.offset) {
            return std::unexpected(QueueError::Truncated);
        }

        // The fingerprint before the cursor, which also seeds the ring.
        detail::TailRing ring;
        const std::uint64_t tail_start =
            from.offset >= kCursorTailBytes ? from.offset - kCursorTailBytes : 0;
        const auto tail_len = static_cast<std::size_t>(from.offset - tail_start);
        std::array<char, kCursorTailBytes> tail{};
        f.seekg(static_cast<std::streamoff>(tail_start), std::ios::beg);
        if (tail_len > 0) {
            f.read(tail.data(), static_cast<std::streamsize>(tail_len));
            if (static_cast<std::size_t>(f.gcount()) != tail_len) {
                return std::unexpected(QueueError::Unreadable);
            }
        }
        if (detail::fnv1a(tail.data(), tail_len) != from.tail_fnv) {
            return std::unexpected(QueueError::Replaced);
        }
        for (std::size_t i = 0; i < tail_len; ++i) { ring.push(tail[i]); }

        IntentBatch out;
        out.next = from;
        std::vector<std::pair<std::string, std::int64_t>> staged;
        std::string line;
        line.reserve(kMaxIntentLineBytes);
        bool oversized = false;
        // A '\r' is held back: if a '\n' follows it, it was a CRLF ending and
        // is not part of the line -- so it must not count toward the length
        // bound either (R-AB-011).
        bool pending_cr = false;
        std::uint64_t pos = from.offset;
        std::uint64_t line_start = pos;
        std::array<char, 4096> chunk{};
        bool stop = false;

        const auto store = [&](char c) {
            // RULE 11: counted -- an oversized line is not stored past the
            // bound; it is consumed and quarantined as TooLong.
            if (line.size() < kMaxIntentLineBytes) {
                line.push_back(c);
            } else {
                oversized = true;
            }
        };

        while (!stop) {
            f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = f.gcount();
            if (got <= 0) { break; }
            for (std::streamsize k = 0; k < got; ++k) {
                const char ch = chunk[static_cast<std::size_t>(k)];
                if (ch == '\n') {
                    // Bounds are checked BEFORE the line is consumed, so
                    // `more` leaves it for the next call.
                    if (out.accepted.size() >= p_.max_batch
                        || out.lines >= p_.max_lines) {
                        out.more = true;
                        stop = true;
                        break;
                    }
                    ring.push(ch);
                    ++pos;
                    pending_cr = false;          // the CR of a CRLF ending
                    ++out.lines;
                    judge(line, oversized, line_start, now_ns, out, staged);
                    out.next.offset = pos;
                    out.next.tail_fnv = ring.hash();
                    line.clear();
                    oversized = false;
                    line_start = pos;
                    continue;
                }
                ring.push(ch);
                ++pos;
                if (pending_cr) {
                    store('\r');                 // a lone CR inside a line
                    pending_cr = false;
                }
                if (ch == '\r') {
                    pending_cr = true;
                    continue;
                }
                store(ch);
            }
        }
        if (f.bad()) {
            // Nothing staged is committed and the cursor the caller holds is
            // unchanged: the whole call is as if it had not happened.
            return std::unexpected(QueueError::Unreadable);
        }
        if (pending_cr) { store('\r'); }
        out.partial_tail = !out.more && (!line.empty() || oversized);
        for (auto& [id, at] : staged) { seen_.emplace(std::move(id), at); }
        return out;
    }

    /// Un-remember the ids this batch accepted. For a caller whose state save
    /// FAILED: the batch was never acted on, so its requests must be able to
    /// drain again instead of coming back as duplicates (R-AB-008).
    void rollback(const IntentBatch& b) {
        for (const OrderIntent& in : b.accepted) {
            seen_.erase(in.id);
        }
    }

    /// The ids currently remembered, for saving with the cursor.
    [[nodiscard]] std::vector<std::pair<std::string, std::int64_t>>
    remembered() const {
        return {seen_.begin(), seen_.end()};
    }

    /// Restore one remembered id. False if the table is full: the caller must
    /// treat that as corrupt state rather than start with a partial memory.
    [[nodiscard]] bool remember(const std::string& id, std::int64_t at_ns) {
        if (id.empty() || id.size() > kMaxIntentText
            || !detail::valid_intent_timestamp(at_ns)
            || seen_.size() >= p_.dedupe_capacity) {
            return false;
        }
        for (const char c : id) {
            if (!detail::id_char(c)) { return false; }
        }
        return seen_.emplace(id, at_ns).second;
    }

    [[nodiscard]] const DrainPolicy& policy() const noexcept { return p_; }

private:
    explicit IntentDrainer(DrainPolicy p) : p_(p) {}

    void quarantine(IntentBatch& out, std::uint64_t at, QuarantineKind kind,
                    IntentError why) const {
        switch (kind) {
        case QuarantineKind::Unparseable: ++out.refused.unparseable; break;
        case QuarantineKind::TooLong:     ++out.refused.too_long; break;
        case QuarantineKind::Expired:     ++out.refused.expired; break;
        case QuarantineKind::FutureDated: ++out.refused.future_dated; break;
        case QuarantineKind::DuplicateId: ++out.refused.duplicate_ids; break;
        case QuarantineKind::DedupeFull:  ++out.refused.dedupe_full; break;
        }
        if (out.quarantined.size() < p_.max_quarantine_list) {
            out.quarantined.push_back(Quarantined{at, kind, why});
        }
    }

    void judge(std::string& line, bool oversized, std::uint64_t at,
               std::int64_t now_ns, IntentBatch& out,
               std::vector<std::pair<std::string, std::int64_t>>& staged) {
        if (oversized) {
            quarantine(out, at, QuarantineKind::TooLong, IntentError::TooLong);
            return;
        }
        while (!line.empty() && line.back() == ' ') { line.pop_back(); }
        if (line.empty()) { return; }
        auto parsed = parse_intent(line);
        if (!parsed) {
            quarantine(out, at, QuarantineKind::Unparseable, parsed.error());
            return;
        }
        OrderIntent& in = *parsed;
        if (in.at_ns > now_ns && in.at_ns - now_ns > p_.max_future_ns) {
            quarantine(out, at, QuarantineKind::FutureDated, IntentError::BadValue);
            return;
        }
        if (now_ns > in.at_ns && now_ns - in.at_ns > p_.ttl_ns) {
            quarantine(out, at, QuarantineKind::Expired, IntentError::BadValue);
            return;
        }
        // Expire ids before duplicate lookup, not only when the table fills.
        // A valid new request may reuse an id after the old request's TTL.
        std::erase_if(seen_, [&](const auto& e) {
            return now_ns > e.second && now_ns - e.second > p_.ttl_ns;
        });
        // IDENTITY IS THE ID, for as long as it is remembered (R-AB-012). An
        // id is remembered until it ages past the TTL, and only then can be
        // evicted -- at which point the LINE that carried it can no longer be
        // accepted either, because it is expired. A NEW line that re-uses an
        // evicted id is a different request and is accepted; nothing in the
        // schema promises an id is unique for ever.
        bool dup = seen_.contains(in.id);
        for (const auto& s : staged) {
            if (s.first == in.id) { dup = true; }
        }
        if (dup) {
            quarantine(out, at, QuarantineKind::DuplicateId, IntentError::BadValue);
            return;
        }
        if (seen_.size() + staged.size() >= p_.dedupe_capacity) {
            if (seen_.size() + staged.size() >= p_.dedupe_capacity) {
                quarantine(out, at, QuarantineKind::DedupeFull,
                           IntentError::BadValue);
                return;
            }
        }
        staged.emplace_back(in.id, in.at_ns);
        out.accepted.push_back(std::move(in));
    }

    DrainPolicy p_;
    std::unordered_map<std::string, std::int64_t> seen_;
};

// ── persisting the drainer ───────────────────────────────────────────────
//
// Text, so an operator can read it:
//
//   altair-intent-cursor 1
//   offset <n>
//   tail <fnv, decimal>
//   id <id> <at_ns>          (zero or more)
//   end
//
// The `end` line is how a torn state file is told from a short one. Ids are
// `[A-Za-z0-9_-]{1,64}` (order_intent.hpp), so none can contain a space and
// the whitespace-delimited format is unambiguous.

/// Write cursor and remembered ids atomically: temp file, then one rename.
[[nodiscard]] inline std::expected<void, QueueError>
save_drainer_state(const std::string& path, const IntentCursor& c,
                   const IntentDrainer& d) {
    std::ostringstream s;
    s << "altair-intent-cursor 1\n"
      << "offset " << c.offset << "\n"
      << "tail " << c.tail_fnv << "\n";
    for (const auto& [id, at] : d.remembered()) {
        s << "id " << id << ' ' << at << "\n";
    }
    s << "end\n";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { return std::unexpected(QueueError::StateWriteFailed); }
        const std::string bytes = s.str();
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        const bool wrote = static_cast<bool>(f);
        // Checked explicitly rather than left to the destructor, which
        // swallows a close failure (R-AB-013).
        f.close();
        if (!wrote || f.fail()) {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return std::unexpected(QueueError::StateWriteFailed);
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return std::unexpected(QueueError::StateWriteFailed);
    }
    return {};
}

/// Read saved state into `d` and return the cursor.
[[nodiscard]] inline std::expected<IntentCursor, QueueError>
load_drainer_state(const std::string& path, IntentDrainer& d) {
    std::error_code ec;
    const bool there = std::filesystem::exists(path, ec);
    if (ec) {
        // A stat that FAILED is not "no state": treating a permission error
        // or an unavailable share as a fresh start forgets every remembered
        // id and re-accepts in-TTL requests (R-AB-009).
        return std::unexpected(QueueError::Unreadable);
    }
    if (!there) { return std::unexpected(QueueError::NoState); }
    std::ifstream f(path, std::ios::binary);
    if (!f) { return std::unexpected(QueueError::Unreadable); }
    std::string word;
    int version = 0;
    IntentCursor c;
    if (!(f >> word >> version) || word != "altair-intent-cursor" || version != 1
        || !(f >> word >> c.offset) || word != "offset"
        || !(f >> word >> c.tail_fnv) || word != "tail") {
        return std::unexpected(QueueError::CorruptState);
    }
    auto staged_result = IntentDrainer::create(d.policy());
    if (!staged_result) { return std::unexpected(QueueError::CorruptState); }
    IntentDrainer staged = std::move(*staged_result);
    bool ended = false;
    while (f >> word) {
        if (word == "end") {
            ended = true;
            break;
        }
        std::string id;
        std::int64_t at = 0;
        if (word != "id" || !(f >> id >> at) || !staged.remember(id, at)) {
            return std::unexpected(QueueError::CorruptState);
        }
    }
    if (!ended) { return std::unexpected(QueueError::CorruptState); }
    f >> std::ws;
    if (f.bad()) { return std::unexpected(QueueError::Unreadable); }
    if (!f.eof()) { return std::unexpected(QueueError::CorruptState); }
    d = std::move(staged);
    return c;
}

} // namespace altair::oms
