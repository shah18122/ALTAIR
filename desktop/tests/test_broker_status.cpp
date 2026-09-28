// desktop/tests/test_broker_status.cpp -- P11Q-12.
//
// Smit asked why there is no pill saying whether Kite is connected. The reason
// the answer is a file of code rather than a QFileInfo::exists is below: on
// this machine, right now, the session file parses, says "success", carries a
// user id, and is DEAD. A presence check calls that connected.

#include <desktop/broker_status.hpp>
#include <desktop/feed_status.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

using altair::ui::BrokerLink;
using altair::ui::BrokerState;
using altair::ui::broker_label;
using altair::ui::now_ist;
using altair::ui::probe_broker;

/// A session file shaped exactly like Kite's, with a chosen login_time.
/// The token fields are present and filled with obvious junk, because the
/// point of the test is that nothing ever reads them.
QString write_session(const QDir& dir, const QString& name,
                      const QString& login_time,
                      const QString& status = QStringLiteral("success")) {
    const QString path = dir.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(QStringLiteral(
                "{\"status\":\"%1\",\"data\":{"
                "\"user_id\":\"AB1234\",\"user_name\":\"Test\","
                "\"broker\":\"ZERODHA\",\"login_time\":\"%2\","
                "\"access_token\":\"IF_THIS_IS_EVER_READ_THE_TEST_IS_WRONG\","
                "\"refresh_token\":\"ALSO_NEVER_READ\","
                "\"enctoken\":\"NEVER_READ\",\"public_token\":\"NEVER_READ\""
                "}}")
                .arg(status, login_time)
                .toUtf8());
    f.close();
    return path;
}

QString stamp(const QDateTime& t) {
    return t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::printf("P11Q-12 the Kite connection pill\n");

    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QDir dir(tmp.path());
    const QDateTime now = now_ist();

    // ---- 1. THE STATE THIS MACHINE IS ACTUALLY IN -------------------------
    //
    // A file that parses, says success, and was issued yesterday. Kite access
    // tokens are daily, so this is a dead token in a healthy-looking file --
    // and `QFileInfo::exists` returns true for it.
    {
        const QString p = write_session(dir, QStringLiteral("yesterday.json"),
                                        stamp(now.addDays(-1)));
        check(QFile::exists(p),
              "the file exists, parses, and reports status success");
        const BrokerState b = probe_broker(p, true);
#if ALTAIR_HAVE_NET
        check(b.link == BrokerLink::Expired,
              "and it is EXPIRED -- a presence check calls this connected, "
              "which is the whole reason this file exists");
#else
        check(b.link == BrokerLink::NoTransport,
              "and in a build with no HTTP client the pill says NO TRANSPORT, "
              "which is a fact about the BINARY, not about the account");
#endif
        check(b.user_id == QStringLiteral("AB1234")
                  && b.broker == QStringLiteral("ZERODHA"),
              "metadata is read: user id and broker come off the file");
        check(!b.detail.isEmpty(), "and the detail names the next action");
        std::printf("        %s\n        %s\n",
                    qPrintable(broker_label(b.link)), qPrintable(b.detail));
    }

    // ---- 2. THE UI NEVER LOADS A TOKEN ------------------------------------
    //
    // Not fastidiousness. The in-process decision keeps "the UI cannot trade"
    // by construction; a UI holding an access token would still not be able to
    // place an order, but it would hold the one thing that makes placing one
    // possible, in the process most likely to crash and dump core.
    {
        const QString p = write_session(dir, QStringLiteral("tokens.json"),
                                        stamp(now));
        const BrokerState b = probe_broker(p, true);
        const QString all = b.user_id + b.broker + b.detail
                            + broker_label(b.link);
        check(!all.contains(QStringLiteral("IF_THIS_IS_EVER_READ"))
                  && !all.contains(QStringLiteral("NEVER_READ")),
              "no token value reaches ANY field the pill can render -- the "
              "state, the user id, the broker and the detail line are all "
              "metadata");
    }

    // ---- 3. TODAY IS NOT THE SAME CLAIM AS WORKING ------------------------
    //
    // A token issued this morning and revoked at lunch by a login on a phone
    // lives in a file identical to a working one. Amber, not green.
    {
        const QString p = write_session(dir, QStringLiteral("today.json"),
                                        stamp(now));
        const BrokerState b = probe_broker(p, true, 0);
#if ALTAIR_HAVE_NET
        check(b.link == BrokerLink::Unverified && !b.usable(),
              "a session issued TODAY that nothing has called with is "
              "UNVERIFIED, not connected -- a revoked token looks exactly "
              "like this on disk");
        const BrokerState ok = probe_broker(p, true, 1);
        check(ok.link == BrokerLink::Authenticated && ok.usable(),
              "only a successful CALL promotes it, and the call result is "
              "passed in -- nothing here can promote itself");
        const BrokerState no = probe_broker(p, true, -1);
        check(no.link == BrokerLink::Rejected,
              "and an in-date token the broker refused is REJECTED, which is "
              "a different problem from an expired one and needs a different "
              "fix");
#else
        check(b.link == BrokerLink::NoTransport && !b.usable(),
              "and without a transport it stays NO TRANSPORT even for a "
              "session issued today -- this build could not verify it");
#endif
    }

    // ---- 4. THE THINGS THAT ARE NOT A SESSION -----------------------------
    {
        const BrokerState missing =
            probe_broker(dir.filePath(QStringLiteral("nope.json")), true);
        check(missing.detail.contains(QStringLiteral("no ")),
              "a missing file names itself and says to log in");

        const QString bad = dir.filePath(QStringLiteral("garbage.json"));
        { QFile f(bad); f.open(QIODevice::WriteOnly); f.write("not json"); }
        const BrokerState mal = probe_broker(bad, true);
        check(!mal.usable() && mal.detail.contains(QStringLiteral("parse")),
              "a file that is not JSON is refused, not treated as absent");

        const QString failed = write_session(dir, QStringLiteral("failed.json"),
                                             stamp(now),
                                             QStringLiteral("error"));
        const BrokerState fs = probe_broker(failed, true);
        check(!fs.usable(),
              "and a well-formed file recording a FAILED login is not a "
              "session either");
    }

    // ---- 5. A SESSION FROM THE FUTURE -------------------------------------
    //
    // Clock skew or an edited file. Either way the expiry check below it
    // cannot be trusted, so it must not silently pass.
    {
        const QString p = write_session(dir, QStringLiteral("future.json"),
                                        stamp(now.addDays(1)));
        const BrokerState b = probe_broker(p, true);
        check(!b.usable() && b.detail.contains(QStringLiteral("FUTURE")),
              "a login_time in the future is called out rather than treated "
              "as fresh -- it is the input the daily rule depends on");
    }

    // ------------------------------------------------------------------
    // P39-05. THE STATUS PILL NEVER SAYS LIVE OVER A REPLAY.
    //
    // It said "LIVE · Replay" in green -- true about arrival, and a claim
    // about the market. It is the same mistake the Terminal's chain header
    // made the same day, one widget over.
    // ------------------------------------------------------------------
    {
        using altair::ui::FeedSource;
        using altair::ui::Liveness;
        using altair::ui::pill_colour;
        using altair::ui::pill_text;
        const QString rep = pill_text(Liveness::Live, FeedSource::Replay);
        std::printf("    replay arriving -> \"%s\"\n", rep.toUtf8().constData());
        check(rep == QStringLiteral("REPLAYING") && !rep.contains(QStringLiteral("LIVE")),
              "a replay that is arriving says REPLAYING, never LIVE");
        check(pill_colour(Liveness::Live, FeedSource::Replay)
                  != pill_colour(Liveness::Live, FeedSource::Kite),
              "and is not painted in live's green either");
        check(pill_text(Liveness::Live, FeedSource::Kite)
                  == QStringLiteral("LIVE · Kite"),
              "a Kite feed that is arriving is LIVE");
        check(!pill_text(Liveness::Live, FeedSource::Unspecified)
                   .contains(QStringLiteral("LIVE")),
              "a source nobody has named is never LIVE");
        check(pill_text(Liveness::Stale, FeedSource::Replay)
                  .startsWith(QStringLiteral("STALE")),
              "stale is stale on every source");
    }

    // FYERS metadata can establish saved state only, including in a net build.
    {
        using namespace altair::ui;
        const QString path = dir.filePath(QStringLiteral("fyers-test.json"));
        const auto clock = QDateTime::fromString(QStringLiteral("2026-09-23T06:00:00Z"), Qt::ISODate);
        const auto write = [&](const QByteArray& bytes) {
            QFile f(path);
            check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "open FYERS fixture");
            check(f.write(bytes) == bytes.size(), "write complete FYERS fixture");
        };
        write(R"({"status":"success","data":{"logged_at":"2026-09-23T10:00:00+05:30"}})");
        const auto saved = probe_fyers(path, true, true, clock);
        check(saved.link == FyersLink::SessionSaved, "FYERS offset timestamp accepted as saved");
        check(fyers_label(saved.link).contains(QStringLiteral("UNVERIFIED")), "saved FYERS is labelled unverified");
        check(fyers_colour(saved.link) != broker_colour(BrokerLink::Authenticated), "saved FYERS never green");
        check(probe_fyers(path, false, true, clock).link == FyersLink::NotConfigured,
              "FYERS missing client config");
        check(probe_fyers(path, true, false, clock).link == FyersLink::NotConfigured,
              "FYERS missing secret config");
        check(probe_fyers(dir.filePath(QStringLiteral("absent-fyers.json")), true, true, clock).link
              == FyersLink::CredentialsPresent, "configured FYERS still needs a session");
        write(R"({"status":"success","data":{"logged_at":"2026-09-24T06:00:00Z"}})");
        check(probe_fyers(path, true, true, clock).link == FyersLink::Malformed, "FYERS future date refused");
        write(R"({"status":"success","data":{"logged_at":"2026-09-23T05:00:00"}})");
        check(probe_fyers(path, true, true, clock).link == FyersLink::Malformed, "FYERS ambiguous timezone refused");
        write("not json");
        check(probe_fyers(path, true, true, clock).link == FyersLink::Malformed, "FYERS corrupt JSON refused");
        write(QByteArray(65537, ' '));
        check(probe_fyers(path, true, true, clock).link == FyersLink::Malformed, "FYERS oversized metadata refused");
        write(R"({"status":"success","data":{"logged_at":"2020-01-01T06:00:00Z"}})");
        check(probe_fyers(path, true, true, clock).link == FyersLink::SessionSaved,
              "old FYERS metadata remains unverified; no invented token expiry");
    }

    // Runtime pills consume the shared service evidence. Files never promote
    // themselves to authenticated, and an expired evidence window downgrades.
    {
        using namespace altair;
        using namespace altair::broker_view;
        using altair::ui::broker_link_from_evidence;
        constexpr Timestamp at{100};
        BrokerEvidence evidence;
        evidence.session = {BrokerId::ZerodhaKite, 1, 1};
        evidence.auth = AuthStatus::Authenticated;
        evidence.authentication = {Timestamp{90}, Timestamp{110}};
        check(broker_link_from_evidence(evidence, at) == BrokerLink::Authenticated,
              "fresh service evidence drives connected pill");
        evidence.authentication.expires_at = at;
        check(broker_link_from_evidence(evidence, at) == BrokerLink::Expired,
              "expired service evidence cannot remain connected");
        evidence.auth = AuthStatus::SessionSaved;
        check(broker_link_from_evidence(evidence, at) == BrokerLink::Unverified,
              "saved session evidence remains unverified");
        evidence.auth = static_cast<AuthStatus>(255);
        check(broker_link_from_evidence(evidence, at) == BrokerLink::Malformed,
              "unknown service auth state fails closed");
    }

    // The provider-neutral pill keeps auth, feed and account freshness as
    // separate claims for both FYERS and Kite.
    {
        using namespace altair;
        using namespace altair::broker_view;
        using altair::ui::service_broker_label;
        using altair::ui::service_broker_pill;
        constexpr Timestamp evidence_now{1'000};
        BrokerEvidence evidence{};
        evidence.session = {BrokerId::Fyers, 7, 1};
        evidence.auth = AuthStatus::SessionSaved;
        auto pill = service_broker_pill(evidence, evidence_now);
        check(pill.authentication == BrokerLink::Unverified,
              "saved-only FYERS evidence stays unverified");

        evidence.auth = AuthStatus::Authenticated;
        evidence.authentication = {Timestamp{900}, Timestamp{1'100}};
        evidence.feed = altair::broker_view::FeedStatus::Live;
        evidence.origin = DataOrigin::BrokerLive;
        evidence.market_session = evidence.session;
        evidence.market_data = {Timestamp{950}, Timestamp{1'050}};
        evidence.account_session = evidence.session;
        evidence.account_snapshot = {Timestamp{950}, Timestamp{1'050}};
        pill = service_broker_pill(evidence, evidence_now);
        check(pill.authentication == BrokerLink::Authenticated
                  && pill.feed == altair::broker_view::FeedStatus::Live
                  && pill.account_fresh,
              "verified FYERS auth, live data and fresh account remain distinct green claims");
        check(service_broker_label(pill).contains(QStringLiteral("FYERS"))
                  && service_broker_label(pill).contains(QStringLiteral("DATA LIVE")),
              "FYERS service label names provider and data state");

        evidence.market_data.expires_at = evidence_now;
        pill = service_broker_pill(evidence, evidence_now);
        check(pill.feed == altair::broker_view::FeedStatus::Stale,
              "expired market evidence downgrades a claimed-live feed to stale");

        evidence.feed = altair::broker_view::FeedStatus::Disconnected;
        evidence.account_snapshot.expires_at = evidence_now;
        pill = service_broker_pill(evidence, evidence_now);
        check(pill.feed == altair::broker_view::FeedStatus::Disconnected
                  && !pill.account_fresh,
              "disconnected feed and stale account are reported separately");

        evidence.session = {BrokerId::ZerodhaKite, 8, 2};
        evidence.authentication = {Timestamp{900}, Timestamp{1'100}};
        evidence.market_session = evidence.session;
        evidence.market_data = {Timestamp{990}, Timestamp{1'100}};
        evidence.account_session = evidence.session;
        evidence.account_snapshot = {Timestamp{990}, Timestamp{1'100}};
        evidence.feed = altair::broker_view::FeedStatus::Live;
        pill = service_broker_pill(evidence, evidence_now);
        check(pill.authentication == BrokerLink::Authenticated
                  && pill.feed == altair::broker_view::FeedStatus::Live
                  && service_broker_label(pill).startsWith(QStringLiteral("KITE")),
              "reconnected Kite evidence produces a provider-correct verified pill");
    }

    // The desktop projection accepts only fresh helper-produced evidence.
    {
        using namespace altair::broker_view;
        using altair::ui::probe_service_snapshot;
        const QString path = dir.filePath(QStringLiteral("service.json"));
        const auto write = [&](qint64 observed, qint64 expiry, int orders,
                               bool complete) {
            QFile f(path);
            check(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
                  "open service projection fixture");
            const QByteArray body = QStringLiteral(
                R"({"schema_version":1,"broker":"FYERS","fetched_at_unix":%1,"service_epoch":3,"auth_verified_at_unix":%1,"auth_expires_at_unix":%2,"account_expires_at_unix":%2,"feed_status":"disabled","account_id":"FY1","snapshot_complete":%4,"profile_status":200,"funds_status":200,"positions_status":200,"holdings_status":200,"orders_status":%3})")
                .arg(observed).arg(expiry).arg(orders)
                .arg(complete ? QStringLiteral("true") : QStringLiteral("false")).toUtf8();
            check(f.write(body) == body.size(), "write service projection fixture");
        };
        const QDateTime service_now = QDateTime::fromSecsSinceEpoch(1'000, QTimeZone::UTC);
        write(990, 1'010, 200, true);
        const auto fresh = probe_service_snapshot(path, BrokerId::Fyers, service_now);
        check(fresh && fresh->authentication == BrokerLink::Authenticated
                  && fresh->account_fresh && !fresh->account_partial,
              "fresh complete verified projection drives the FYERS pill");
        write(990, 1'010, 200, false);
        const auto partial = probe_service_snapshot(path, BrokerId::Fyers, service_now);
        check(partial && partial->account_partial
                  && altair::ui::service_broker_label(*partial).contains("PARTIAL"),
              "partial account response is visible instead of appearing empty");
        write(900, 950, 200, true);
        const auto old = probe_service_snapshot(path, BrokerId::Fyers, service_now);
        check(old && old->authentication == BrokerLink::Expired && !old->account_fresh,
              "old service file can never produce a connected or fresh badge");
        QFile malformed(path);
        check(malformed.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "open malformed service projection");
        malformed.write("{\"schema_version\":1,\"broker\":\"FYERS\"}");
        malformed.close();
        check(!probe_service_snapshot(path, BrokerId::Fyers, service_now),
              "incomplete service projection fails closed");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
