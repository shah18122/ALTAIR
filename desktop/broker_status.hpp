// desktop/broker_status.hpp -- the KITE pill: is the broker session real?
//
// P11Q-12.
//
// "CONNECTED" IS NOT ONE QUESTION, AND THE FILE EXISTING ANSWERS NONE OF THEM.
//
// Smit asked why there is no pill saying whether Kite is connected. There was
// a LIVE pill already (P11Q-05b) and it answers a different question -- is data
// ARRIVING -- which is why it did not fill this gap. A replay session shows
// LIVE while the broker is untouched, and a valid Kite session shows NO DATA
// until something subscribes. Both are correct. Neither says whether we can
// talk to the broker.
//
// The obvious version of this pill is `QFileInfo::exists("kite_session.json")`
// -- green if present. That check was already in `feed_status.hpp`'s wiring
// table, and **it is wrong today**: the file on this machine parses perfectly,
// carries `status: success`, and was issued 2026-09-04. Kite access tokens are
// DAILY. It is a dead token in a healthy-looking file, which is the same shape
// as P12-02's torn snapshot -- the file still PARSES, so parsing is not the
// check.
//
// So the states are separated, because they need different actions from a
// human and collapsing them into a green dot destroys exactly the information
// that tells you what to do:
//
//   NoTransport        this binary has no HTTP client linked at all. Nothing
//                      about a session file can make it able to connect, and a
//                      pill claiming CONNECTED here would be a lie about the
//                      BINARY, not about the account.
//   NoCredentials      ALTAIR_KITE_API_KEY is unset. Cannot even attempt.
//   NoSession          credentials, no session file. Do the browser login.
//   Malformed          a file that is not a session, or one whose login_time
//                      is in the FUTURE -- a clock skew or an edited file, and
//                      either way not something to trust.
//   Expired            issued on an earlier IST day. **The common case, and
//                      the one the naive check gets wrong.** Log in again.
//   Unverified         issued TODAY, but nothing has actually called the API
//                      with it. A token can be revoked server-side by logging
//                      in elsewhere, and the file cannot know.
//   Authenticated      a call succeeded. Only the transport may set this.
//   Rejected           a call was refused. Distinct from Expired: the token
//                      was in date and the broker still said no.
//
// THE UI READS THE SESSION'S METADATA AND NEVER THE TOKEN.
//
// `data/kite_session.json` holds a live trading credential. This file parses
// four fields -- status, login_time, user_id, broker -- and deliberately does
// not read `access_token`, `refresh_token`, `enctoken` or `public_token`.
//
// That is not fastidiousness. CLAUDE.md's in-process decision keeps one safety
// property by construction: THE UI CANNOT TRADE. It is kept by a CMake
// allow-list that stops `desktop/` linking `oms/` or `broker/`. A UI that
// loaded an access token into a QString would still not be able to place an
// order today, but it would be holding the one thing that makes placing one
// possible, in the process most likely to crash and dump core. The pill can
// answer every question above from metadata alone, so it does.

#pragma once

#include <types/broker_state.hpp>

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTimeZone>

#include <array>
#include <cstdint>
#include <optional>

// Defined by the build when the vcpkg `net` feature is present.
#ifndef ALTAIR_HAVE_NET
#define ALTAIR_HAVE_NET 0
#endif

namespace altair::ui {

/// FYERS is the configured primary broker.  The desktop deliberately checks
/// only whether the two non-secret environment variables are present; it does
/// not load an access token or claim that a network call succeeded.  That
/// keeps the read-only UI honest while making the primary route visible.
using FyersLink = altair::broker_view::AuthStatus;

struct FyersState {
    FyersLink link = FyersLink::NotConfigured;
    QString detail;
};

/// Cold metadata probe. Time is UTC/offset-aware; values are never verified here.
/// Reads session JSON bytes transiently; separate metadata publication is pending.
[[nodiscard]] inline FyersState probe_fyers(const QString& path, bool client_id,
                                           bool secret, const QDateTime& now) {
    if (!client_id || !secret) {
        return {FyersLink::NotConfigured,
                QStringLiteral("set ALTAIR_FYERS_CLIENT_ID and ALTAIR_FYERS_SECRET locally; secrets are never shown or stored by the UI.")};
    }

    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        // Metadata is tiny. Refuse both oversized files and growth during read.
        constexpr qint64 max_bytes = 64 * 1024;
        const QByteArray raw = f.read(max_bytes + 1);
        f.close();
        if (raw.size() > max_bytes) {
            return {FyersLink::Malformed, QStringLiteral("FYERS session file exceeds 64 KiB; link again.")};
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        const QJsonObject root = doc.object();
        const QJsonObject data = root.value(QStringLiteral("data")).toObject();
        const QString logged = data.value(QStringLiteral("logged_at")).toString();
        const QDateTime issued = QDateTime::fromString(logged, Qt::ISODate);
        const bool explicit_zone = logged.endsWith(QLatin1Char('Z'))
            || (logged.size() >= 6 && logged.at(logged.size() - 3) == QLatin1Char(':')
                && (logged.at(logged.size() - 6) == QLatin1Char('+')
                    || logged.at(logged.size() - 6) == QLatin1Char('-')));
        if (err.error == QJsonParseError::NoError && doc.isObject()
            && root.value(QStringLiteral("status")).toString() == QStringLiteral("success")
            && issued.isValid() && explicit_zone && now.isValid()
            && issued.toMSecsSinceEpoch() > 0 && issued <= now) {
            return {FyersLink::SessionSaved,
                    QStringLiteral("FYERS session saved at %1 UTC; connection and expiry are unverified.")
                        .arg(issued.toUTC().toString(Qt::ISODate))};
        }
        return {FyersLink::Malformed,
                QStringLiteral("Invalid or future-dated FYERS session metadata; link again.")};
    }
    return {FyersLink::CredentialsPresent,
            QStringLiteral("No FYERS session today: Brokers → Log in to FYERS.")};
}

/// Probe the session metadata. App credentials are NOT judged here: they
/// live in the OS vault (Brokers → Log in · FYERS → App credentials), which
/// this process never reads, so an unset environment variable says nothing.
/// The state is the session: saved, missing or malformed.
[[nodiscard]] inline FyersState probe_fyers() {
#ifdef ALTAIR_FYERS_SESSION_FILE
    const QString path = QStringLiteral(ALTAIR_FYERS_SESSION_FILE);
#else
    const QString path = QStringLiteral("data/fyers_session.json");
#endif
    return probe_fyers(path, true, true, QDateTime::currentDateTimeUtc());
}

[[nodiscard]] inline QString fyers_label(FyersLink link) {
    switch (link) {
    case FyersLink::SessionSaved:
        return QStringLiteral("FYERS — SESSION SAVED · UNVERIFIED");
    case FyersLink::CredentialsPresent:
        return QStringLiteral("FYERS — NOT LOGGED IN");
    case FyersLink::NotConfigured:
        return QStringLiteral("FYERS — PRIMARY · SETUP REQUIRED");
    case FyersLink::Malformed:
        return QStringLiteral("FYERS — BAD SESSION FILE");
    default:
        return QStringLiteral("FYERS — UNVERIFIED");
    }
}

[[nodiscard]] inline QColor fyers_colour(FyersLink link) {
    return link == FyersLink::Malformed
               ? QColor(0xC0, 0x39, 0x2B)
               : (link == FyersLink::CredentialsPresent || link == FyersLink::SessionSaved)
                   ? QColor(0xB9, 0x77, 0x0B)
                   : QColor(0x7F, 0x8C, 0x8D);
}

/// Ordered by how much is known, worst first. Not a severity ranking -- see
/// `broker_colour`, where `NoTransport` is grey because it is a property of
/// this build rather than a fault in the account.
enum class BrokerLink : std::uint8_t {
    NoTransport = 0,
    NoCredentials,
    NoSession,
    Malformed,
    Expired,
    Unverified,
    Rejected,
    Authenticated
};

[[nodiscard]] inline QString broker_label(BrokerLink b) {
    switch (b) {
    case BrokerLink::NoTransport:   return QStringLiteral("KITE — NO TRANSPORT");
    case BrokerLink::NoCredentials: return QStringLiteral("KITE — NO API KEY");
    case BrokerLink::NoSession:     return QStringLiteral("KITE — NOT LOGGED IN");
    case BrokerLink::Malformed:     return QStringLiteral("KITE — BAD SESSION FILE");
    case BrokerLink::Expired:       return QStringLiteral("KITE — SESSION EXPIRED");
    case BrokerLink::Unverified:    return QStringLiteral("KITE — TOKEN UNVERIFIED");
    case BrokerLink::Rejected:      return QStringLiteral("KITE — REFUSED");
    case BrokerLink::Authenticated: return QStringLiteral("KITE — CONNECTED");
    }
    return QStringLiteral("KITE — ?");
}

/// GREY for "this build cannot", AMBER for "you must do something", RED for
/// "the broker said no", GREEN only for a call that actually succeeded.
///
/// `Unverified` is amber and not green on purpose. A token issued this morning
/// and revoked at lunch by a login on the phone lives in a file that looks
/// exactly like a working one.
[[nodiscard]] inline QColor broker_colour(BrokerLink b) {
    switch (b) {
    case BrokerLink::Authenticated: return QColor(0x1B, 0x8A, 0x4B);
    case BrokerLink::Rejected:
    case BrokerLink::Malformed:     return QColor(0xC0, 0x39, 0x2B);
    case BrokerLink::Expired:
    case BrokerLink::NoSession:
    case BrokerLink::Unverified:    return QColor(0xB9, 0x77, 0x0B);
    case BrokerLink::NoCredentials:
    case BrokerLink::NoTransport:
    default:                        return QColor(0x7F, 0x8C, 0x8D);
    }
}

/// What the pill knows. Everything here is metadata; no field can hold a
/// credential, which is a property of the STRUCT and not of the care taken by
/// whoever writes the next caller.
struct BrokerState {
    BrokerLink link = BrokerLink::NoSession;
    QString user_id;       ///< e.g. "AB1234". Not secret; it is on every note.
    QString broker;        ///< "ZERODHA"
    QDateTime issued_ist;  ///< invalid when unknown
    /// One line naming the next action, not a restatement of the label.
    QString detail;

    [[nodiscard]] bool usable() const noexcept {
        return link == BrokerLink::Authenticated;
    }
};

/// Map service-published evidence to the legacy Kite pill state. This is the
/// runtime path: a session file may explain setup, but only fresh shared
/// evidence can produce Authenticated. UTC ns arrive from the app clock.
[[nodiscard]] inline BrokerLink
broker_link_from_evidence(const broker_view::BrokerEvidence& evidence,
                          Timestamp now) noexcept {
    using broker_view::AuthStatus;
    switch (evidence.auth) {
    case AuthStatus::Authenticated:
        return broker_view::authenticated(evidence, now)
            ? BrokerLink::Authenticated : BrokerLink::Expired;
    case AuthStatus::Expired:            return BrokerLink::Expired;
    case AuthStatus::Rejected:           return BrokerLink::Rejected;
    case AuthStatus::Malformed:          return BrokerLink::Malformed;
    case AuthStatus::NoTransport:        return BrokerLink::NoTransport;
    case AuthStatus::NotConfigured:      return BrokerLink::NoCredentials;
    case AuthStatus::CredentialsPresent:
    case AuthStatus::LinkRequired:       return BrokerLink::NoSession;
    case AuthStatus::SessionSaved:
    case AuthStatus::Unverified:         return BrokerLink::Unverified;
    }
    return BrokerLink::Malformed;
}

/// Provider-neutral status for the two broker pills. Authentication, market
/// data and account freshness stay separate so a healthy login cannot paint a
/// stale feed green, and a fresh feed cannot imply that funds are current.
struct ServiceBrokerPill {
    broker_view::BrokerId broker{broker_view::BrokerId::None};
    BrokerLink authentication{BrokerLink::Malformed};
    broker_view::FeedStatus feed{broker_view::FeedStatus::Disabled};
    bool account_fresh{};
    bool account_partial{};
};

[[nodiscard]] inline ServiceBrokerPill
service_broker_pill(const broker_view::BrokerEvidence& evidence,
                    Timestamp now) noexcept {
    ServiceBrokerPill out{};
    out.broker = evidence.session.broker;
    out.authentication = broker_link_from_evidence(evidence, now);
    out.feed = evidence.feed;
    if (out.feed == broker_view::FeedStatus::Live
        && !broker_view::live_data(evidence, now)) {
        out.feed = broker_view::FeedStatus::Stale;
    }
    out.account_fresh = evidence.account_session == evidence.session
        && broker_view::fresh(evidence.account_snapshot, now);
    return out;
}

[[nodiscard]] inline QString broker_name(broker_view::BrokerId broker) {
    switch (broker) {
    case broker_view::BrokerId::Fyers: return QStringLiteral("FYERS");
    case broker_view::BrokerId::ZerodhaKite: return QStringLiteral("KITE");
    case broker_view::BrokerId::None: break;
    }
    return QStringLiteral("BROKER");
}

[[nodiscard]] inline QString feed_suffix(broker_view::FeedStatus feed) {
    using broker_view::FeedStatus;
    switch (feed) {
    case FeedStatus::Live:         return QStringLiteral("DATA LIVE");
    case FeedStatus::Connecting:   return QStringLiteral("DATA CONNECTING");
    case FeedStatus::Stale:        return QStringLiteral("DATA STALE");
    case FeedStatus::Disconnected: return QStringLiteral("DATA DISCONNECTED");
    case FeedStatus::Rejected:     return QStringLiteral("DATA REJECTED");
    case FeedStatus::Error:        return QStringLiteral("DATA ERROR");
    case FeedStatus::Disabled:     return QStringLiteral("DATA OFF");
    }
    return QStringLiteral("DATA ?");
}

[[nodiscard]] inline QString service_broker_label(const ServiceBrokerPill& pill) {
    QString auth;
    switch (pill.authentication) {
    case BrokerLink::Authenticated: auth = QStringLiteral("VERIFIED"); break;
    case BrokerLink::Expired:       auth = QStringLiteral("EXPIRED"); break;
    case BrokerLink::Rejected:      auth = QStringLiteral("REFUSED"); break;
    case BrokerLink::Unverified:    auth = QStringLiteral("UNVERIFIED"); break;
    case BrokerLink::NoSession:     auth = QStringLiteral("NOT LINKED"); break;
    case BrokerLink::NoCredentials: auth = QStringLiteral("SETUP REQUIRED"); break;
    case BrokerLink::NoTransport:   auth = QStringLiteral("NO TRANSPORT"); break;
    case BrokerLink::Malformed:     auth = QStringLiteral("INVALID"); break;
    }
    return QStringLiteral("%1 — %2 · %3 · ACCOUNT %4")
        .arg(broker_name(pill.broker), auth, feed_suffix(pill.feed),
             pill.account_partial ? QStringLiteral("PARTIAL")
             : pill.account_fresh ? QStringLiteral("FRESH")
                                : QStringLiteral("STALE/ABSENT"));
}

/// Read a credential-free service projection emitted only after a validated
/// account call. The short service TTL is authoritative: an old file can
/// explain history but can never paint a connected badge green.
[[nodiscard]] inline std::optional<ServiceBrokerPill>
probe_service_snapshot(const QString& path, broker_view::BrokerId expected,
                       const QDateTime& now_utc = QDateTime::currentDateTimeUtc()) {
    QFile file(path);
    constexpr qint64 kMaxBytes = 2 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kMaxBytes)
        return std::nullopt;
    const QByteArray raw = file.read(kMaxBytes + 1);
    if (raw.size() > kMaxBytes) return std::nullopt;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !now_utc.isValid()) return std::nullopt;
    const QJsonObject root = document.object();
    const QString expected_name = expected == broker_view::BrokerId::Fyers
        ? QStringLiteral("FYERS") : QStringLiteral("ZERODHA_KITE");
    const qint64 observed = root.value(QStringLiteral("fetched_at_unix")).toInteger();
    const qint64 verified = root.value(QStringLiteral("auth_verified_at_unix")).toInteger();
    const qint64 auth_expires = root.value(QStringLiteral("auth_expires_at_unix")).toInteger();
    const qint64 account_expires = root.value(QStringLiteral("account_expires_at_unix")).toInteger();
    const qint64 epoch = root.value(QStringLiteral("service_epoch")).toInteger();
    const qint64 now = now_utc.toSecsSinceEpoch();
    if (root.value(QStringLiteral("schema_version")).toInt() != 1
        || root.value(QStringLiteral("broker")).toString() != expected_name
        || root.value(QStringLiteral("account_id")).toString().isEmpty()
        || epoch <= 0 || observed <= 0 || verified != observed
        || auth_expires <= verified || account_expires <= observed
        || observed > now + 5) return std::nullopt;
    ServiceBrokerPill pill{};
    pill.broker = expected;
    pill.authentication = now < auth_expires
        ? BrokerLink::Authenticated : BrokerLink::Expired;
    const QString feed = root.value(QStringLiteral("feed_status")).toString();
    pill.feed = feed == QStringLiteral("live") && now < auth_expires
        ? broker_view::FeedStatus::Live : broker_view::FeedStatus::Disabled;
    pill.account_fresh = now < account_expires;
    if (expected == broker_view::BrokerId::Fyers
        && root.contains(QStringLiteral("snapshot_complete"))) {
        if (!root.value(QStringLiteral("snapshot_complete")).isBool()) return std::nullopt;
        pill.account_partial = !root.value(QStringLiteral("snapshot_complete")).toBool();
        return pill;
    }
    constexpr std::array<const char*, 4> common_sections{
        "profile_status", "positions_status", "holdings_status", "orders_status"};
    if (root.value(expected == broker_view::BrokerId::Fyers
                       ? QStringLiteral("funds_status")
                       : QStringLiteral("margins_status")).toInt() != 200)
        pill.account_partial = true;
    for (const char* section : common_sections) {
        if (root.value(QLatin1String(section)).toInt() != 200) {
            pill.account_partial = true;
            break;
        }
    }
    return pill;
}

/// IST is UTC+05:30 with no DST, so "today in IST" is a fixed offset from UTC.
/// Taken from UTC rather than local time deliberately: the answer must not
/// change because the laptop is in a different timezone.
[[nodiscard]] inline QDateTime now_ist() {
    return QDateTime::currentDateTimeUtc().addSecs(19800).toUTC();
}

/// Read the session file's METADATA. Never reads a token -- see the header.
///
/// `api_key_present` is passed in rather than read here so that the one place
/// that touches an environment variable stays outside the UI's parsing code.
/// `verified` is what a successful API call sets; nothing in this file can
/// promote a session to Authenticated on its own, for the same reason
/// `FeedStatus` has no way to set itself Live.
/// Both parameters are [[maybe_unused]] because they are read only in the
/// networked configuration: a build with no HTTP client cannot check a
/// credential it has no way to use, and cannot have made the call that sets
/// `verified`. They stay in the signature so the two builds share one
/// interface -- a probe whose ARITY changed with a feature flag would push
/// that #if out into every caller.
[[nodiscard]] inline BrokerState probe_broker(const QString& session_path,
                                              [[maybe_unused]] bool api_key_present,
                                              [[maybe_unused]] int verified = 0) {
    BrokerState s;

#if !ALTAIR_HAVE_NET
    s.link = BrokerLink::NoTransport;
    s.detail = QStringLiteral(
        "this build has no HTTP client linked (vcpkg `net` feature absent). "
        "Build --preset net to talk to Kite.");
    // Fall through: the session file is still WORTH READING, because knowing
    // the token is stale is useful even in a build that could not use a fresh
    // one. The link state stays NoTransport regardless of what is found.
#else
    if (!api_key_present) {
        s.link = BrokerLink::NoCredentials;
        s.detail = QStringLiteral("ALTAIR_KITE_API_KEY is not set in this "
                                  "environment.");
        return s;
    }
#endif

    QFile f(session_path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
#if ALTAIR_HAVE_NET
        s.link = BrokerLink::NoSession;
#endif
        s.detail = QStringLiteral("no %1 — log in on Brokers → Log in to Kite.")
                       .arg(session_path);
        return s;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
#if ALTAIR_HAVE_NET
        s.link = BrokerLink::Malformed;
#endif
        s.detail = QStringLiteral("session file did not parse: %1")
                       .arg(err.errorString());
        return s;
    }
    const QJsonObject root = doc.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();

    // The four metadata fields, and nothing else. access_token,
    // refresh_token, enctoken and public_token are NOT read.
    s.user_id = data.value(QStringLiteral("user_id")).toString();
    s.broker = data.value(QStringLiteral("broker")).toString();
    const QString status = root.value(QStringLiteral("status")).toString();
    const QString login = data.value(QStringLiteral("login_time")).toString();

    if (status != QStringLiteral("success")) {
#if ALTAIR_HAVE_NET
        s.link = BrokerLink::Malformed;
#endif
        s.detail = QStringLiteral("session file records status \"%1\", not "
                                  "success.").arg(status);
        return s;
    }

    // Kite stamps login_time in IST with no offset, so it is parsed as a naive
    // instant and compared against IST -- never against the local clock.
    s.issued_ist = QDateTime::fromString(login, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (s.issued_ist.isValid()) {
        // now_ist uses UTC-tagged IST calendar fields. Use the same tag here;
        // fromString otherwise interprets them in the computer's local zone.
        s.issued_ist = QDateTime(s.issued_ist.date(), s.issued_ist.time(), QTimeZone::UTC);
    }
    if (!s.issued_ist.isValid()) {
#if ALTAIR_HAVE_NET
        s.link = BrokerLink::Malformed;
#endif
        s.detail = QStringLiteral("login_time \"%1\" is not a timestamp.")
                       .arg(login);
        return s;
    }

    const QDateTime now = now_ist();
    if (s.issued_ist > now.addSecs(300)) {
        // Five minutes of slack for clock skew; beyond that a session issued
        // in the future is an edited file or a wrong clock, and both mean the
        // expiry check below cannot be trusted.
#if ALTAIR_HAVE_NET
        s.link = BrokerLink::Malformed;
#endif
        s.detail = QStringLiteral("login_time is in the FUTURE (%1 IST). "
                                  "Clock skew or an edited file.")
                       .arg(s.issued_ist.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        return s;
    }

    // THE DAILY RULE. A Kite access token is issued per login and dies the
    // next morning, so a session stamped on an earlier IST date is dead no
    // matter how well-formed the file is. Compared on the CALENDAR DATE rather
    // than a 24-hour window, which is the conservative direction: it can say
    // "log in again" an hour early, and it can never say "connected" about a
    // token the broker has already retired.
    if (s.issued_ist.date() != now.date()) {
        const qint64 hours = s.issued_ist.secsTo(now) / 3600;
#if ALTAIR_HAVE_NET
        s.link = BrokerLink::Expired;
#endif
        s.detail = QStringLiteral("issued %1 IST (%2 h ago) — Kite tokens are "
                                  "daily. Log in again.")
                       .arg(s.issued_ist.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                       .arg(hours);
        return s;
    }

#if ALTAIR_HAVE_NET
    if (verified > 0) {
        s.link = BrokerLink::Authenticated;
        s.detail = QStringLiteral("a live call succeeded at %1 IST.")
                       .arg(now.toString(QStringLiteral("HH:mm:ss")));
    } else if (verified < 0) {
        s.link = BrokerLink::Rejected;
        s.detail = QStringLiteral("the token is in date and the broker refused "
                                  "it — it was probably revoked by a login "
                                  "elsewhere.");
    } else {
        s.link = BrokerLink::Unverified;
        s.detail = QStringLiteral("issued %1 IST today, but no call has used it "
                                  "yet. A token revoked elsewhere looks exactly "
                                  "like this.")
                       .arg(s.issued_ist.toString(QStringLiteral("HH:mm")));
    }
#else
    s.detail += QStringLiteral(" Session file is from %1 IST today.")
                    .arg(s.issued_ist.toString(QStringLiteral("HH:mm")));
#endif
    return s;
}

} // namespace altair::ui
