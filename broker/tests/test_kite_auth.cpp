// P2-10 acceptance tests for broker/sha256.hpp and broker/kite_session.hpp.
//
// Run with no arguments for the test suite. Pass an API key to print the real
// browser login URL for that key:
//
//     altair_kite_auth_test <api_key>
//
// NO SECRET APPEARS IN THIS FILE and none should ever be added. The checksum
// is proven with dummy values; the real one is computed at runtime from an
// environment variable, by code that never logs it.
//
// No check description here may contain the substring FAIL.

#include <broker/kite_session.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

namespace {

bool hex_is(const char* s, const char* expect)
{
    return std::strcmp(s, expect) == 0;
}

void hash_of(const char* s, char* out)
{
    const Sha256Digest d = sha256(s, std::strlen(s));
    sha256_hex(d, out);
}

// ── 1 ────────────────────────────────────────────────────────────────────
// FIPS 180-4 / NIST. The multi-block cases matter most: a SHA-256 whose
// padding is wrong only past 55 bytes passes "abc" perfectly and then fails
// on every real input, which is exactly the shape of an API key plus a
// request token plus a secret.
void nist_vectors()
{
    std::printf("\n1 nist_vectors\n");
    char hex[kSha256HexChars + 1];

    hash_of("", hex);
    check(hex_is(hex, "e3b0c44298fc1c149afbf4c8996fb924"
                      "27ae41e4649b934ca495991b7852b855"),
          "the empty string -- the pure-padding case");

    hash_of("abc", hex);
    check(hex_is(hex, "ba7816bf8f01cfea414140de5dae2223"
                      "b00361a396177a9cb410ff61f20015ad"),
          "\"abc\" -- one block, the vector everyone tests");

    // 56 bytes: the first length that forces a SECOND block, because the
    // 8-byte length field no longer fits after the 0x80 byte.
    hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    check(hex_is(hex, "248d6a61d20638b8e5c026930c3e6039"
                      "a33ce45964ff2167f6ecedd419db06c1"),
          "56 bytes -- the first length that spills into a second block");

    // 112 bytes: two full blocks plus padding.
    hash_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
            "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", hex);
    check(hex_is(hex, "cf5b16a778af8380036ce59e7b049237"
                      "0b249b11e8f07a51afac45037afee9d1"),
          "112 bytes -- multi-block with padding in its own block");

    // One million 'a'. Proves the 64-bit BIT length, not a byte count: at
    // 8'000'000 bits this is the case where a byte/bit mix-up first shows.
    Sha256 s;
    const char chunk[1000] = {};
    char a[1000];
    std::memset(a, 'a', sizeof(a));
    (void)chunk;
    for (int i = 0; i < 1000; ++i) {
        s.update(a, sizeof(a));
    }
    const Sha256Digest d = s.finish();
    sha256_hex(d, hex);
    check(hex_is(hex, "cdc76e5c9914fb9281a1c7e284d73e67"
                      "f1809a48a497200e046d39ccc7112cd0"),
          "one million 'a' -- proves the length field counts BITS, not bytes");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void streaming_matches_one_shot()
{
    std::printf("\n2 streaming_matches_one_shot\n");

    const char* whole = "the quick brown fox jumps over the lazy dog, twice, "
                        "to push this comfortably past a single 64-byte block";
    char one[kSha256HexChars + 1];
    hash_of(whole, one);

    // Fed one byte at a time, so every buffer-boundary path is exercised.
    Sha256 s;
    for (const char* p = whole; *p != '\0'; ++p) {
        s.update(p, 1);
    }
    char streamed[kSha256HexChars + 1];
    sha256_hex(s.finish(), streamed);
    check(hex_is(streamed, one),
          "byte-at-a-time streaming matches the one-shot hash exactly");

    // And in three uneven chunks straddling the 64-byte boundary.
    Sha256 t;
    t.update(whole, 13);
    t.update(whole + 13, 51);          // ends exactly on the block boundary
    t.update(whole + 64, std::strlen(whole) - 64);
    char chunked[kSha256HexChars + 1];
    sha256_hex(t.finish(), chunked);
    check(hex_is(chunked, one),
          "and so does a split landing exactly on the block boundary");

    Sha256 u;
    u.update(whole, std::strlen(whole));
    (void)u.finish();
    u.reset();
    u.update("abc", 3);
    char after_reset[kSha256HexChars + 1];
    sha256_hex(u.finish(), after_reset);
    check(hex_is(after_reset, "ba7816bf8f01cfea414140de5dae2223"
                              "b00361a396177a9cb410ff61f20015ad"),
          "reset() returns the object to a clean initial state");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The concatenation order is load-bearing and unguessable from the error Kite
// returns, which is generic.
void session_checksum()
{
    std::printf("\n3 session_checksum\n");

    const char* key = "aaaaaaaaaaaaaaaa";
    const char* req = "bbbbbbbbbbbbbbbb";
    const char* sec = "cccccccccccccccc";

    char sum[kSha256HexChars + 1];
    const auto r = kite_session_checksum(key, req, sec, sum, sizeof(sum));
    check(r.has_value(), "the checksum computes");

    // Independently: SHA256 of the three concatenated, in that order.
    char expect[kSha256HexChars + 1];
    hash_of("aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbcccccccccccccccc", expect);
    check(hex_is(sum, expect),
          "and equals SHA256(api_key + request_token + api_secret) -- "
          "no separator, no encoding");

    check(std::strlen(sum) == kSha256HexChars, "64 hex characters");
    bool lower = true;
    for (const char* p = sum; *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'F') {
            lower = false;
        }
    }
    check(lower,
          "and LOWERCASE -- Kite compares the string, and Go's %x is lowercase");

    // Order matters. Any other arrangement is a different hash, and Kite's
    // reply would not say which.
    char swapped[kSha256HexChars + 1];
    (void)kite_session_checksum(key, sec, req, swapped, sizeof(swapped));
    check(!hex_is(swapped, sum),
          "swapping request_token and secret gives a DIFFERENT checksum -- "
          "the order is not a detail");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void refuses_incomplete_input()
{
    std::printf("\n4 refuses_incomplete_input\n");
    char sum[kSha256HexChars + 1];

    const auto a = kite_session_checksum("", "req", "sec", sum, sizeof(sum));
    check(!a.has_value() && a.error() == KiteAuthError::MissingApiKey,
          "an empty api_key is refused");
    const auto b = kite_session_checksum("key", "", "sec", sum, sizeof(sum));
    check(!b.has_value() && b.error() == KiteAuthError::MissingRequestToken,
          "an empty request_token is refused");
    const auto c = kite_session_checksum("key", "req", "", sum, sizeof(sum));
    check(!c.has_value() && c.error() == KiteAuthError::MissingSecret,
          "an empty secret is refused -- hashing an empty secret would "
          "produce a perfectly valid-looking checksum that is always wrong");
    const auto d = kite_session_checksum("key", "req", "sec", sum, 10);
    check(!d.has_value() && d.error() == KiteAuthError::BufferTooSmall,
          "and a short output buffer is refused rather than truncated");

    const auto e = kite_session_checksum("key", "req", "sec", sum, sizeof(sum));
    check(e.has_value(), "a complete set succeeds");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void login_url()
{
    std::printf("\n5 login_url\n");
    char url[kKiteLoginUrlMax];

    const auto r = kite_login_url("abcdef1234567890", url, sizeof(url));
    check(r.has_value(), "the login URL builds");
    check(std::strcmp(url, "https://kite.zerodha.com/connect/login"
                           "?api_key=abcdef1234567890&v=3") == 0,
          "and matches Zerodha's own GetLoginURL, v=3 included");
    check(r.has_value() && *r == std::strlen(url), "the returned length is right");

    const auto e = kite_login_url("", url, sizeof(url));
    check(!e.has_value() && e.error() == KiteAuthError::MissingApiKey,
          "an empty api_key is refused");

    char tiny[8];
    const auto t = kite_login_url("abcdef1234567890", tiny, sizeof(tiny));
    check(!t.has_value() && t.error() == KiteAuthError::BufferTooSmall,
          "and a short buffer is refused, not silently truncated into a URL "
          "that would fail with a confusing error");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2) {
        // Print the real login URL for a given API key. The key is not a
        // secret -- it travels in this URL in the user's address bar.
        char url[kKiteLoginUrlMax];
        const auto r = kite_login_url(argv[1], url, sizeof(url));
        if (!r) {
            std::fprintf(stderr, "bad api_key\n");
            return 2;
        }
        std::printf("\nOpen this in a browser and log in:\n\n  %s\n\n", url);
        std::printf("Zerodha will redirect to your registered redirect_url\n"
                    "carrying ?request_token=... Nothing here can do that step\n"
                    "for you: it needs your user ID, password and TOTP.\n\n");
        return 0;
    }

    std::printf("altair broker kite auth tests\n");
    nist_vectors();
    streaming_matches_one_shot();
    session_checksum();
    refuses_incomplete_input();
    login_url();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
