// desktop/broker_status.hpp -- the broker pill: is the FYERS session real?
//
// P11Q-12.
//
// "CONNECTED" IS NOT ONE QUESTION, AND THE FILE EXISTING ANSWERS NONE OF THEM.
//
// A LIVE pill (P11Q-05b) answers a different question -- is data ARRIVING. A
// replay session shows LIVE while the broker is untouched, and a valid session
// shows NO DATA until something subscribes. Neither says whether we can talk
// to the broker, and a session file that PARSES can still hold a dead token:
// FYERS access tokens are DAILY.
//
// So the states are separated, because they need different actions from a
// human and collapsing them into a green dot destroys exactly the information
// that tells you what to do:
//
//   NoTransport        this binary has no HTTP client linked at all.
//   NoCredentials      the FYERS app credentials are not set up.
//   NoSession          credentials, no session today. Log in (or let the
//                      08:30 auto-login run).
//   Malformed          a file that is not a session, or one stamped in the
//                      FUTURE -- not something to trust.
//   Expired            issued on an earlier IST day. Log in again.
//   Unverified         issued TODAY, but nothing has actually called the API
//                      with it. A token can be revoked server-side by logging
//                      in elsewhere, and the file cannot know.
//   Authenticated      a call succeeded. Only the transport may set this.
//   Rejected           a call was refused. Distinct from Expired: the token
//                      was in date and the broker still said no.
//
// THE UI READS THE SESSION'S METADATA AND NEVER THE TOKEN. THE UI CANNOT
// TRADE: a CMake allow-list stops `desktop/` linking `oms/` or `broker/`, and
// the pill answers every question above from metadata alone.

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

/// Map service-published evidence to the pill state. This is the
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

/// Provider-neutral status for the broker pill. Authentication, market
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
    if (expected != broker_view::BrokerId::Fyers) return std::nullopt;
    const QString expected_name = QStringLiteral("FYERS");
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
    if (root.contains(QStringLiteral("snapshot_complete"))) {
        if (!root.value(QStringLiteral("snapshot_complete")).isBool()) return std::nullopt;
        pill.account_partial = !root.value(QStringLiteral("snapshot_complete")).toBool();
        return pill;
    }
    constexpr std::array<const char*, 4> common_sections{
        "profile_status", "positions_status", "holdings_status", "orders_status"};
    if (root.value(QStringLiteral("funds_status")).toInt() != 200)
        pill.account_partial = true;
    for (const char* section : common_sections) {
        if (root.value(QLatin1String(section)).toInt() != 200) {
            pill.account_partial = true;
            break;
        }
    }
    return pill;
}

} // namespace altair::ui
