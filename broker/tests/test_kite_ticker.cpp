// P32-07 acceptance tests for broker/kite_ticker.hpp.
//
// WHAT IS TESTABLE HERE AND WHAT IS NOT.
//
// The socket is not. It needs a live daily token and an open market, and a
// test that skipped itself without one would pass by not running -- the same
// reasoning P26-01 gives for not testing the Link Kite QProcess half.
//
// What IS testable is everything that decides what goes on the wire and what
// comes off it in a log: the URL, the two control messages, and the redaction.
// Those are where the damage would be. A malformed subscribe is a feed that
// silently carries nothing; a URL printed unredacted is a live trading token
// in a terminal scrollback.
//
// No check description here may contain the substring FAIL.

#include <broker/kite_ticker.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

int main()
{
    std::printf("P32-07 -- the ticker's wire, without a socket\n");

    // ── 1. the URL ───────────────────────────────────────────────────────
    std::printf("\n1 the_url_carries_the_credential_because_kite_says_so\n");
    {
        const auto t = kite_ticker_target("KEY123", "TOK456");
        check(t.has_value(), "a key and a token build a target");
        if (t) {
            check(t->find("api_key=KEY123") != std::string::npos
                      && t->find("access_token=TOK456") != std::string::npos,
                  "both are query parameters -- Kite authenticates the ticker "
                  "that way and there is no header form");
        }
        check(!kite_ticker_target("", "TOK").has_value(),
              "an empty api_key is REFUSED rather than sent");
        check(!kite_ticker_target("KEY", "").has_value(),
              "and so is an empty access_token -- an unauthenticated "
              "connection that looks like a working one is the failure to "
              "avoid here");
    }

    // ── 2. redaction ─────────────────────────────────────────────────────
    //
    // THE ONE THING THAT MUST NOT LEAK. The credential is in the URL because
    // that is Kite's protocol, so every path that could print a URL has to go
    // through this.
    std::printf("\n2 redaction_keeps_the_length_and_not_the_value\n");
    {
        const std::string url =
            "/?api_key=abcdef123456&access_token=zyxwvu987654321";
        const std::string red = redact_ws_url(url);
        std::printf("    %s\n", red.c_str());
        check(red.find("abcdef123456") == std::string::npos,
              "the api_key is gone");
        check(red.find("zyxwvu987654321") == std::string::npos,
              "and so is the access_token");
        check(red.find("api_key=************") != std::string::npos,
              "the LENGTH survives, so a truncated or doubled key is still "
              "visible as wrong without the value being shown");
        check(red.find("access_token=") != std::string::npos,
              "and the parameter names survive, so the URL is still "
              "recognisable as the thing it is");
    }

    // ── 3. the control messages ──────────────────────────────────────────
    //
    // A malformed subscribe does not error. It produces a socket that is open,
    // authenticated, and carrying nothing -- which is indistinguishable from a
    // closed market.
    std::printf("\n3 the_two_control_messages\n");
    {
        const std::vector<std::uint32_t> toks{256265u, 260105u};
        const std::string sub = kite_subscribe_message(toks);
        const std::string mod = kite_mode_message(TickerMode::Full, toks);
        std::printf("    %s\n    %s\n", sub.c_str(), mod.c_str());
        check(sub == "{\"a\":\"subscribe\",\"v\":[256265,260105]}",
              "subscribe is exactly the shape Kite documents");
        check(mod == "{\"a\":\"mode\",\"v\":[\"full\",[256265,260105]]}",
              "and so is mode -- note the nested array, which is the part "
              "that is easy to get wrong and produces a silent no-op");
        check(kite_subscribe_message({}) == "{\"a\":\"subscribe\",\"v\":[]}",
              "an empty list produces valid JSON rather than a stray comma; "
              "kite_ticker_run refuses it before it gets here");
    }

    // ── 4. the modes ─────────────────────────────────────────────────────
    std::printf("\n4 mode_names\n");
    {
        check(std::string(ticker_mode_name(TickerMode::Ltp)) == "ltp"
                  && std::string(ticker_mode_name(TickerMode::Quote)) == "quote"
                  && std::string(ticker_mode_name(TickerMode::Full)) == "full",
              "all three modes name themselves as Kite spells them");
    }

    // ── 5. the errors say what to do ─────────────────────────────────────
    //
    // A daily token is the single most likely thing to be wrong on this
    // endpoint, and "upgrade failed" does not tell anybody that.
    std::printf("\n5 the_upgrade_error_names_the_likely_cause\n");
    {
        const std::string t = ticker_error_text(TickerError::UpgradeFailed);
        std::printf("    %s\n", t.c_str());
        check(t.find("token") != std::string::npos
                  && t.find("daily") != std::string::npos,
              "a refused upgrade points at the daily token, which is what it "
              "almost always is -- an error naming only the HTTP layer sends "
              "the reader to debug the wrong thing");
    }

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
