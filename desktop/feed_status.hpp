// desktop/feed_status.hpp -- the LIVE pill, and what it is not allowed to say.
//
// P11Q-05b.
//
// A CONNECTED SOCKET IS NOT A LIVE FEED.
//
// This is the card. The obvious pill tracks the socket: connected means LIVE,
// green. Then the exchange goes quiet, or the decoder thread wedges, or the
// broker stops publishing for one symbol while the TCP connection stays up
// perfectly, and the pill says LIVE over a book that stopped moving four
// minutes ago.
//
// A dashboard that says LIVE is making a claim about the DATA, not about the
// socket. So `FeedStatus` derives its state from when the last tick actually
// arrived, and the socket state can only ever DOWNGRADE that -- never promote
// it. `Live` requires a tick within `stale_after`; past that it is `Stale`
// regardless of what the connection thinks, and the age is shown next to the
// pill so "how stale" is a number rather than a colour.
//
// THE AGE IS MEASURED ON THE RECEIVE CLOCK, NOT THE EXCHANGE CLOCK.
//
// The one place in this UI that does NOT read the tick's exchange timestamp.
// Rule 7 says a strategy reads time off the tick -- that is about decisions.
// Staleness is a question about US: has data arrived recently. An exchange
// timestamp cannot answer it, because a feed replaying yesterday emits
// yesterday's timestamps forever and would look permanently fresh by its own
// clock. `feed/tick.hpp` carries `recv_ts` for exactly this, and says so:
// "For latency measurement and stall detection (P2-05), never for trading
// decisions."
//
// AND THE WIRING TABLE SAYS WHAT IS ACTUALLY BUILT.
//
// Smit asked for a toggle between Kite and XTS "showing what all is wired up".
// The honest version of that is not a toggle with two equal options; it is a
// table of what exists, because the two are nowhere near equal:
//
//   Kite   spec parser, decoder, order TRANSLATION -- but no transport, no
//          session, and P1-07 blocked on credentials
//   XTS    P1-05, P2-03 and P4-06 all deferred; nothing is built
//
// `wiring()` returns that table so the panel renders facts rather than two
// radio buttons implying a choice the tree cannot honour.

#pragma once

#include "broker_status.hpp"

#include <core/time/timestamp.hpp>

#include <QColor>
#include <QFileInfo>
#include <QString>

#include <cstdint>
#include <vector>

// Defined by the build when the vcpkg `net` feature is present.
#ifndef ALTAIR_HAVE_NET
#define ALTAIR_HAVE_NET 0
#endif

namespace altair::ui {

enum class FeedSource : std::uint8_t {
    /// Phantom default. Not "none" -- nobody has said yet.
    Unspecified = 0,
    /// Replaying a recorded or synthetic session.
    Replay,
    Kite,
    Xts
};

[[nodiscard]] inline QString feed_name(FeedSource s) {
    switch (s) {
    case FeedSource::Replay: return QStringLiteral("Replay");
    case FeedSource::Kite:   return QStringLiteral("Kite");
    case FeedSource::Xts:    return QStringLiteral("XTS");
    case FeedSource::Unspecified:
    default:                 return QStringLiteral("—");
    }
}

enum class Liveness : std::uint8_t {
    /// Nothing has arrived yet. Distinct from Stale: we have no evidence
    /// either way, and saying "stale" would be a claim about the feed.
    NoData = 0,
    /// A tick arrived within the staleness window.
    Live,
    /// The link may be up, but nothing has arrived recently.
    Stale,
    /// The transport is known to be down.
    Disconnected
};

[[nodiscard]] inline QString liveness_label(Liveness l) {
    switch (l) {
    case Liveness::Live:         return QStringLiteral("LIVE");
    case Liveness::Stale:        return QStringLiteral("STALE");
    case Liveness::Disconnected: return QStringLiteral("DISCONNECTED");
    case Liveness::NoData:
    default:                     return QStringLiteral("NO DATA");
    }
}

[[nodiscard]] inline QColor liveness_colour(Liveness l) {
    switch (l) {
    case Liveness::Live:         return QColor(0x1B, 0x8A, 0x4B);
    case Liveness::Stale:        return QColor(0xB9, 0x77, 0x0B);
    case Liveness::Disconnected: return QColor(0xC0, 0x39, 0x2B);
    case Liveness::NoData:
    default:                     return QColor(0x7F, 0x8C, 0x8D);
    }
}

/// Tracks whether data is actually arriving.
///
/// `transport_up` can only downgrade the answer. There is deliberately no way
/// to set the state to Live directly: it is derived, every time, from when a
/// tick last arrived.
class FeedStatus {
public:
    explicit FeedStatus(std::int64_t stale_after_ns = 5'000'000'000LL) noexcept
        : stale_after_ns_(stale_after_ns) {}

    void set_source(FeedSource s) noexcept { source_ = s; }
    [[nodiscard]] FeedSource source() const noexcept { return source_; }

    /// The transport's own opinion. It can say "down"; it cannot say "live".
    void set_transport_up(bool up) noexcept { transport_up_ = up; }

    /// Call when a tick ARRIVES, with the local receive clock -- not the
    /// exchange timestamp on the tick. See the header.
    void observe_arrival(std::int64_t recv_ns) noexcept {
        last_arrival_ns_ = recv_ns;
        seen_ = true;
        ++arrivals_;
    }

    [[nodiscard]] std::uint64_t arrivals() const noexcept { return arrivals_; }

    /// Nanoseconds since the last arrival, or -1 when nothing has arrived.
    [[nodiscard]] std::int64_t age_ns(std::int64_t now_ns) const noexcept {
        return seen_ ? now_ns - last_arrival_ns_ : -1;
    }

    [[nodiscard]] Liveness liveness(std::int64_t now_ns) const noexcept {
        if (!transport_up_) {
            return Liveness::Disconnected;
        }
        if (!seen_) {
            // Not Stale. We have no evidence, and "stale" would be a claim
            // about the feed rather than about our knowledge of it.
            return Liveness::NoData;
        }
        return (now_ns - last_arrival_ns_) <= stale_after_ns_ ? Liveness::Live
                                                              : Liveness::Stale;
    }

private:
    std::int64_t stale_after_ns_;
    std::int64_t last_arrival_ns_ = 0;
    std::uint64_t arrivals_ = 0;
    FeedSource source_ = FeedSource::Unspecified;
    bool transport_up_ = false;
    bool seen_ = false;
};

// ---------------------------------------------------------------------------
// What is actually wired
// ---------------------------------------------------------------------------

enum class WiringState : std::uint8_t {
    Unspecified = 0,
    /// Built, tested, and usable now.
    Built,
    /// The code exists and is tested, but something it needs is absent.
    BlockedOnInput,
    /// Deliberately not built yet.
    NotBuilt,
    /// Removed from the plan. Distinct from NotBuilt, which is work still
    /// queued -- a reader planning around this table would leave room for a
    /// venue that is never coming.
    Withdrawn
};

[[nodiscard]] inline QString wiring_label(WiringState w) {
    switch (w) {
    case WiringState::Withdrawn:      return QStringLiteral("withdrawn");
    case WiringState::Built:          return QStringLiteral("built");
    case WiringState::BlockedOnInput: return QStringLiteral("blocked");
    case WiringState::NotBuilt:       return QStringLiteral("not built");
    case WiringState::Unspecified:
    default:                          return QStringLiteral("—");
    }
}

[[nodiscard]] inline QColor wiring_colour(WiringState w) {
    switch (w) {
    case WiringState::Built:          return QColor(0x1B, 0x8A, 0x4B);
    case WiringState::BlockedOnInput: return QColor(0xB9, 0x77, 0x0B);
    case WiringState::NotBuilt:       return QColor(0xC0, 0x39, 0x2B);
    // GREY, not red. Red says "missing and needed"; withdrawn is
    // neither, and colouring it like a gap keeps it on somebody's
    // list forever.
    case WiringState::Withdrawn:      return QColor(0x7F, 0x8C, 0x8D);
    case WiringState::Unspecified:
    default:                          return QColor(0x7F, 0x8C, 0x8D);
    }
}

struct WiringRow {
    QString broker;
    QString stage;
    WiringState state = WiringState::Unspecified;
    QString detail;
};

/// The wiring table.
///
/// Hand-maintained ON PURPOSE, and every row names the card that owns it. A
/// version that probed the build for symbols would be clever and would answer
/// a different question -- "does this link" rather than "can this trade" --
/// and the second is what the panel is for. When a row changes, the card that
/// changed it changes this line, and the diff shows it.
/// Is there a Kite session file on disk?
///
/// PRESENCE ONLY. It deliberately does not read the token, does not check the
/// expiry and does not call Kite. An access token is valid until the next
/// morning, so a file that exists may well be dead -- and the panel says
/// exactly that rather than implying a live connection it has not tested.
/// Claiming "connected" from a file's existence is how a dashboard tells you
/// the feed is up while nothing is arriving.
[[nodiscard]] inline bool session_present() {
#ifdef ALTAIR_SESSION_FILE
    return QFileInfo::exists(QStringLiteral(ALTAIR_SESSION_FILE));
#else
    return QFileInfo::exists(QStringLiteral("data/kite_session.json"));
#endif
}

/// The wiring row's state, DERIVED. Only a session that is present, well
/// formed and issued today can reach `Built`; anything else is amber or red,
/// which is the point -- the row is read to decide whether to trust the rest
/// of the window.
[[nodiscard]] inline WiringState broker_wiring_state() {
#ifdef ALTAIR_SESSION_FILE
    const BrokerState b = probe_broker(QStringLiteral(ALTAIR_SESSION_FILE),
                                       true);
#else
    const BrokerState b = probe_broker(QStringLiteral("data/kite_session.json"),
                                       true);
#endif
    switch (b.link) {
    case BrokerLink::Authenticated: return WiringState::Built;
    case BrokerLink::Malformed:
    case BrokerLink::Rejected:      return WiringState::NotBuilt;
    default:                        return WiringState::BlockedOnInput;
    }
}

[[nodiscard]] inline QString broker_wiring_note() {
#ifdef ALTAIR_SESSION_FILE
    const BrokerState b = probe_broker(QStringLiteral(ALTAIR_SESSION_FILE),
                                       true);
#else
    const BrokerState b = probe_broker(QStringLiteral("data/kite_session.json"),
                                       true);
#endif
    return broker_label(b.link) + QStringLiteral(" \u2014 ") + b.detail;
}

[[nodiscard]] inline std::vector<WiringRow> wiring() {
    return {
        {QStringLiteral("Kite"), QStringLiteral("Instrument master (P1-04)"),
         WiringState::Built,
         QStringLiteral("CSV parser + three-way reconciliation, tested")},
        {QStringLiteral("Kite"), QStringLiteral("Tick decoder (P2-02)"),
         WiringState::Built,
         QStringLiteral("binary quote/full mode, tested against vectors")},
        {QStringLiteral("Kite"), QStringLiteral("Order translation (P4-05)"),
         WiringState::Built,
         QStringLiteral("builds the POST body; deliberately does NOT send it")},
        // ---- THESE THREE ARE DETECTED, NOT ASSERTED ---------------------
        //
        // They were hard-coded "NotBuilt" and "no session", and both became
        // FALSE the moment the net preset built and Smit logged in -- so the
        // panel spent a morning stating the opposite of the truth. That is
        // the same stale-literal failure P11Q-07b fixed on the Markov row and
        // P11Q-08 fixed on a card reference, and a wiring panel is a worse
        // place for it than either: it is the screen you check BEFORE
        // believing anything else on the window.
        //
        // ALTAIR_HAVE_NET is a compile-time fact and the session file is a
        // runtime one, so each is read where it lives rather than typed here.
        {QStringLiteral("Kite"), QStringLiteral("HTTPS transport (P2-10c)"),
#if ALTAIR_HAVE_NET
         WiringState::Built,
         QStringLiteral("boost-beast + OpenSSL, verified peer + hostname; "
                        "GET and form POST")},
#else
         WiringState::NotBuilt,
         QStringLiteral("this build has no `net` feature; configure with "
                        "--preset net")},
#endif
        // ---- PRESENCE WAS NEVER THE QUESTION ----------------------------
        //
        // This row used to say "present / not present" and explain in prose
        // that presence is not validity. That is an honest caption on a
        // misleading state: a reader scanning a column of green sees green.
        //
        // `probe_broker` (P11Q-12) answers the question the row is actually
        // asking, from the file's METADATA and never its token -- and on this
        // machine it turns the row AMBER, because the session parses, says
        // `success`, and was issued on an earlier IST day. Kite tokens are
        // daily. The old check called that "Built".
        {QStringLiteral("Kite"), QStringLiteral("Session / access token"),
         broker_wiring_state(), broker_wiring_note()},
        {QStringLiteral("Kite"), QStringLiteral("Historical candles (P2-12)"),
#if ALTAIR_HAVE_NET
         WiringState::Built,
         QStringLiteral("chunked, coverage-checked, dry-run by default; "
                        "filled dataset/ with 2.7M bars")},
#else
         WiringState::NotBuilt,
         QStringLiteral("parser and chunker are built and tested; the fetch "
                        "needs the `net` feature")},
#endif
        {QStringLiteral("Kite"), QStringLiteral("Margin fetch (P1-07)"),
         WiringState::BlockedOnInput,
         QStringLiteral("blocked on credentials")},
        // XTS IS WITHDRAWN, NOT PENDING, AND THE DIFFERENCE MATTERS ON A
        // WIRING PAGE.
        //
        // "Deferred" reads as work still queued, and a reader planning around
        // this table would leave room for a second venue. Smit removed XTS
        // from the plan on 2026-09-04: Kite is the only venue.
        //
        // The rows stay rather than vanishing, because the ENUMERATORS stay --
        // `kFeedSourceCount` sizes arrays in feed/failover.hpp, in
        // ContractSpec::token[] and in the plausibility gate, so `FeedSource`
        // still has an Xts value and someone reading the code will find it. A
        // page that showed no XTS at all would leave that unexplained.
        {QStringLiteral("XTS"), QStringLiteral("Instrument master (P1-05)"),
         WiringState::Withdrawn,
         QStringLiteral("withdrawn 2026-09-04 — Kite is the only venue")},
        {QStringLiteral("XTS"), QStringLiteral("Socket.IO decoder (P2-03)"),
         WiringState::Withdrawn,
         QStringLiteral("withdrawn 2026-09-04")},
        {QStringLiteral("XTS"), QStringLiteral("Execution adapter (P4-06)"),
         WiringState::Withdrawn,
         QStringLiteral("withdrawn 2026-09-04. `oms/throttle.hpp` REFUSES "
                        "Venue::Xts rather than falling back to Kite, so "
                        "nothing routes there by accident — that is the "
                        "property that mattered and it is unchanged")},
        {QStringLiteral("Replay"), QStringLiteral("Session replayer (P0-09)"),
         WiringState::Built,
         QStringLiteral("forward-only, no peek(); drives this window")},
        {QStringLiteral("Replay"), QStringLiteral("Bar loader (P11Q-06)"),
         WiringState::Built,
         QStringLiteral("real NIFTY + India VIX from dataset/")},
    };
}

/// Can this broker carry an order today? Derived from the table rather than
/// asserted separately, so the two cannot disagree.
[[nodiscard]] inline bool can_trade(const QString& broker) {
    bool saw_any = false;
    for (const WiringRow& r : wiring()) {
        if (r.broker != broker) {
            continue;
        }
        saw_any = true;
        if (r.state != WiringState::Built) {
            return false;
        }
    }
    return saw_any;
}

} // namespace altair::ui
