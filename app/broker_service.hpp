// app/broker_service.hpp -- the broker-service poll loop (P2-09).
//
// broker/service_runtime.hpp is the deterministic control plane: it decides
// WHEN to poll, accepts a validated reply, applies backoff, and publishes
// immutable evidence a reader (the desktop pills, P2-11) consumes. What it does
// not do is TALK to anything -- it has no transport, by design. This file is the
// app/ half that drives it: poll -> fetch -> publish, over a transport seam.
//
// ONE CONTROL PATH FOR THE BINARY AND THE TEST (rule 6).
//
// `Transport` is a template parameter, not a live socket baked in. The shipping
// binary hands it an adapter over broker/https_client.hpp + the account-snapshot
// parser (dry-run by default, exactly as P2-07/P2-08 already are); the test
// hands it a scripted fake. Both drive the SAME pump(), so a bug in the poll ->
// accept -> backoff sequencing is caught by the test rather than only in
// production. Nothing here can place an order: it includes no oms/ header and
// the transport fetches READ-ONLY account snapshots.
//
// TIME IS A PARAMETER (rule 7). pump() takes `now`; this file reads no clock.
// The binary passes a real clock reading; the test passes a fixed sequence, so
// the poll cadence, the freshness deadline and the backoff are all reproducible.
//
// HELPER DEATH DOES NOT REWRITE HISTORY. When the transport reports the helper
// is down, the last published snapshot keeps its ORIGINAL observation time --
// the runtime guarantees it, and test_broker_service asserts it, because a
// stale snapshot wearing a fresh timestamp is the lie that makes a dead feed
// look alive.

#pragma once

#include <broker/service_runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace altair::app {

/// What a single fetch attempt produced. The snapshot is meaningful only for
/// Published; the other outcomes carry no data and drive an invalidation.
enum class FetchOutcome : std::uint8_t {
    Published,   ///< a validated read-only account snapshot is in `snapshot`
    HelperDown,  ///< the fetch helper was unreachable or died mid-request
    Rejected     ///< the broker refused the session (revoked/expired creds)
};

struct FetchResult {
    FetchOutcome outcome{FetchOutcome::HelperDown};
    broker_view::AccountSnapshot snapshot{};
};

/// The result of one pump() cycle. Idle means the poll clock said "not yet".
enum class PumpResult : std::uint8_t {
    Idle,        ///< poll returned Wait / AlreadyRunning; nothing fetched
    Published,   ///< a snapshot was accepted and published
    Refused,     ///< a reply arrived but failed validation; backoff applied
    HelperDown,  ///< the helper was unavailable; last evidence preserved
    Rejected,    ///< the broker rejected the session
    Revoked      ///< the session is revoked; polling has stopped
};

/// Drives one broker's read-only account service. One writer owns it; readers
/// consume immutable publications through read(). `Transport` must provide:
///     FetchResult fetch(const broker_view::SessionKey& session,
///                       std::uint64_t epoch, Timestamp now);
template <class Transport, std::size_t LogCapacity = 128>
class BrokerService {
public:
    BrokerService(broker_view::BrokerId provider, Transport transport,
                  broker::BrokerServicePolicy policy = {}) noexcept
        : runtime_(provider, policy), transport_(std::move(transport)) {}

    /// Record a saved session and arm the first poll. Verification is still
    /// pending until a reply is accepted; link() alone never claims connected.
    /// UNIT: now in ns. Returns false if the session is not linkable.
    [[nodiscard]] bool link(broker_view::SessionKey session, Timestamp now) noexcept {
        if (!runtime_.link(session, now)) { return false; }
        session_ = session;   // the runtime keeps its own copy privately; the
        return true;          // transport needs it to address the fetch.
    }

    /// One poll/fetch/publish cycle at `now`. Fetches only when the runtime's
    /// bounded poll clock says Start; otherwise Idle. UNIT: now in ns.
    [[nodiscard]] PumpResult pump(Timestamp now) noexcept {
        switch (runtime_.poll(now)) {
        case broker::PollDecision::Revoked:
            return PumpResult::Revoked;
        case broker::PollDecision::Wait:
        case broker::PollDecision::AlreadyRunning:
            return PumpResult::Idle;
        case broker::PollDecision::Start:
            break;
        }
        const std::uint64_t epoch = runtime_.poll_epoch();
        const FetchResult reply = transport_.fetch(session_, epoch, now);
        switch (reply.outcome) {
        case FetchOutcome::Published:
            return runtime_.accept(reply.snapshot, epoch, now)
                       ? PumpResult::Published : PumpResult::Refused;
        case FetchOutcome::HelperDown:
            runtime_.helper_failed(now);
            return PumpResult::HelperDown;
        case FetchOutcome::Rejected:
            runtime_.credentials_rejected(now);
            return PumpResult::Rejected;
        }
        return PumpResult::Idle;  // unreachable; the switch is exhaustive
    }

    /// Stop the service: the session is revoked and no further poll will start.
    void revoke(Timestamp now) noexcept { runtime_.revoke(now); }

    [[nodiscard]] std::expected<std::uint64_t, broker::StateStoreError>
    read(broker_view::BrokerEvidence& value) const noexcept {
        return runtime_.read(value);
    }
    [[nodiscard]] std::expected<std::uint64_t, broker::StateStoreError>
    read(broker_view::AccountSnapshot& value) const noexcept {
        return runtime_.read(value);
    }

    [[nodiscard]] broker::SnapshotConnection state() const noexcept {
        return runtime_.state();
    }
    [[nodiscard]] broker_view::BrokerId provider() const noexcept {
        return runtime_.provider();
    }
    [[nodiscard]] std::size_t log_size() const noexcept { return runtime_.log_size(); }
    [[nodiscard]] std::uint64_t omitted_logs() const noexcept {
        return runtime_.omitted_logs();
    }
    [[nodiscard]] const broker_view::BrokerLogEvent* log_at(std::size_t i) const noexcept {
        return runtime_.log_at(i);
    }

private:
    broker::BrokerServiceRuntime<LogCapacity> runtime_;
    Transport transport_;
    broker_view::SessionKey session_{};
};

} // namespace altair::app
