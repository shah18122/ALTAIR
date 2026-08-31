// P0-08b acceptance tests for core/config/toml_source.
//
// Only built under the `vcpkg` preset, because it links tomlplusplus.
//
// No check description here may contain the substring FAIL.

#include <config/toml_source.hpp>

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

ConfigSnapshot g_snap;

auto load(const char* toml)
{
    g_snap = ConfigSnapshot{};
    return load_toml(toml, std::strlen(toml), g_snap);
}

std::int64_t int_at(const char* key, std::int64_t fallback = -12345)
{
    const auto h = g_snap.find(key);
    if (!h) {
        return fallback;
    }
    const auto v = g_snap.get_int(*h);
    return v ? *v : fallback;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void nested_tables_flatten_with_dots()
{
    std::printf("\n1 nested_tables_flatten_with_dots\n");

    const auto r = load(
        "top = 1\n"
        "[feed]\n"
        "staleness_ms = 250\n"
        "[feed.kite]\n"
        "max_subscriptions = 3000\n");

    check(r.has_value(), "the document parses");
    check(int_at("top") == 1, "a top-level scalar keeps its bare key");
    check(int_at("feed.staleness_ms") == 250,
          "a table member becomes feed.staleness_ms");
    check(int_at("feed.kite.max_subscriptions") == 3000,
          "and a nested table becomes feed.kite.max_subscriptions");
    check(r.has_value() && r->loaded == 3, "three scalars loaded");
    check(!g_snap.find("staleness_ms").has_value(),
          "the unqualified name does NOT resolve -- flattening must not "
          "collapse two tables' keys onto one another");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// D1. ConfigSnapshot rides a seqlock, so every entry is fixed-size and
// trivially copyable. A string cannot go in one.
void strings_are_skipped_and_counted()
{
    std::printf("\n2 strings_are_skipped_and_counted\n");

    const auto r = load(
        "[feed.kite]\n"
        "api_key_env = \"ALTAIR_KITE_API_KEY\"\n"
        "max_subscriptions = 3000\n");

    check(r.has_value(), "a document with strings still loads");
    check(r.has_value() && r->skipped_string == 1,
          "the string is COUNTED, not silently dropped and not an error");
    check(r.has_value() && r->loaded == 1, "and the integer beside it loads");
    check(int_at("feed.kite.max_subscriptions") == 3000, "with the right value");
    check(!g_snap.find("feed.kite.api_key_env").has_value(),
          "the string key is absent from the snapshot");
    check(r.has_value()
              && std::strcmp(r->first_skipped, "feed.kite.api_key_env") == 0,
          "and the report NAMES the first one, so a message can say which");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void scalar_arrays_flatten_with_an_index()
{
    std::printf("\n3 scalar_arrays_flatten_with_an_index\n");

    const auto r = load(
        "[feed.xts]\n"
        "events = [1501, 1502, 1505]\n");

    check(r.has_value() && r->loaded == 3, "three array elements load");
    check(int_at("feed.xts.events.0") == 1501, "events.0 is 1501");
    check(int_at("feed.xts.events.1") == 1502, "events.1 is 1502");
    check(int_at("feed.xts.events.2") == 1505,
          "events.2 is 1505 -- the XTS subscriber genuinely needs these, and "
          "dropping them silently would be worse than an odd key shape");

    // An array of tables is a different shape and is not invented into keys.
    const auto t = load("[[strategy]]\nname_id = 1\n[[strategy]]\nname_id = 2\n");
    check(t.has_value(), "an array of tables parses");
    check(t.has_value() && t->skipped_other >= 1,
          "and is counted as skipped rather than flattened into a guess");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void types_map_correctly()
{
    std::printf("\n4 types_map_correctly\n");

    const auto r = load(
        "an_int = 42\n"
        "a_neg = -7\n"
        "a_bool = true\n"
        "a_real = 0.25\n"
        "big = 9223372036854775807\n");

    check(r.has_value() && r->loaded == 5, "five scalars load");
    check(int_at("an_int") == 42, "an integer");
    check(int_at("a_neg") == -7, "a negative integer");
    check(int_at("big") == 9223372036854775807LL,
          "and INT64_MAX survives without narrowing -- toml++ integers are "
          "int64, which is exactly what the snapshot holds");

    const auto hb = g_snap.find("a_bool");
    check(hb.has_value() && g_snap.get_bool(*hb).has_value()
              && *g_snap.get_bool(*hb),
          "a boolean");
    const auto hr = g_snap.find("a_real");
    check(hr.has_value() && g_snap.get_real(*hr).has_value()
              && *g_snap.get_real(*hr) == 0.25,
          "and a float, which lands as a double -- rule 3's analytics-only "
          "carve-out, so a config value that is MONEY must be written as "
          "integer paise in the TOML itself");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void bad_toml_is_an_error_not_a_partial_load()
{
    std::printf("\n5 bad_toml_is_an_error_not_a_partial_load\n");

    const auto r = load("good = 1\nthis is not toml [[[\n");
    check(!r.has_value() && r.error() == TomlError::ParseFailed,
          "a malformed document is ParseFailed");
    check(!g_snap.find("good").has_value(),
          "and NOTHING was loaded -- a half-parsed config is worse than none, "
          "because the missing half looks like a default");

    const auto e = load("");
    check(e.has_value() && e->loaded == 0,
          "an empty document is valid TOML and loads zero keys");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void over_long_keys_are_refused_not_truncated()
{
    std::printf("\n6 over_long_keys_are_refused_not_truncated\n");

    // kMaxConfigKeyLen is 47. Build something past it.
    const auto r = load(
        "[aaaaaaaaaaaaaaaaaaaaaaaaa]\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb = 1\n");
    check(!r.has_value() && r.error() == TomlError::KeyTooLong,
          "a flattened key past kMaxConfigKeyLen is REFUSED");
    check(g_snap.size() == 0,
          "and nothing partial was stored -- a truncated key silently collides "
          "with another and a strategy reads someone else's limit");

    const auto ok = load("[short]\nkey = 1\n");
    check(ok.has_value() && int_at("short.key") == 1, "a short key is fine");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void merge_semantics()
{
    std::printf("\n7 merge_semantics\n");

    g_snap = ConfigSnapshot{};
    const char* base = "[risk]\nmax_lots = 10\nkill_switch = false\n";
    const char* over = "[risk]\nmax_lots = 3\n";

    check(load_toml(base, std::strlen(base), g_snap).has_value(), "base loads");
    check(int_at("risk.max_lots") == 10, "max_lots is 10");

    check(load_toml(over, std::strlen(over), g_snap).has_value(),
          "a second document loads into the SAME snapshot");
    check(int_at("risk.max_lots") == 3, "and overrides max_lots to 3");

    const auto k = g_snap.find("risk.kill_switch");
    check(k.has_value(), "while a key absent from the override SURVIVES -- "
                         "this is a merge, not a replace");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void the_real_altair_toml_loads()
{
    std::printf("\n8 the_real_altair_toml_loads\n");

    g_snap = ConfigSnapshot{};
    const auto r = load_toml_file("config/altair.toml", g_snap);
    if (!r) {
        // The test may run from a different working directory; try one up.
        const auto r2 = load_toml_file("../../config/altair.toml", g_snap);
        check(r2.has_value(),
              "the project's own config/altair.toml parses (from either path)");
        if (!r2) {
            return;
        }
        std::printf("       loaded %zu, strings skipped %zu, other %zu, "
                    "tables %zu\n",
                    r2->loaded, r2->skipped_string, r2->skipped_other,
                    r2->tables);
        check(r2->loaded > 0, "and yields real keys");
        check(r2->skipped_string > 0,
              "with its strings skipped as designed -- env-var names and URLs "
              "are read by cold startup code, not from the hot snapshot");
        return;
    }

    std::printf("       loaded %zu, strings skipped %zu, other %zu, tables %zu\n",
                r->loaded, r->skipped_string, r->skipped_other, r->tables);
    check(r->loaded > 0, "the project's own config/altair.toml yields real keys");
    check(r->skipped_string > 0,
          "with its strings skipped as designed -- env-var names and URLs are "
          "read by cold startup code, not from the hot snapshot");
    check(r->tables > 1, "and several tables were walked");

    const auto missing = load_toml_file("config/does_not_exist.toml", g_snap);
    check(!missing.has_value() && missing.error() == TomlError::FileNotFound,
          "a missing file is FileNotFound, not an empty success");
}

} // namespace

int main()
{
    std::printf("altair core config toml_source tests\n");
    nested_tables_flatten_with_dots();
    strings_are_skipped_and_counted();
    scalar_arrays_flatten_with_an_index();
    types_map_correctly();
    bad_toml_is_an_error_not_a_partial_load();
    over_long_keys_are_refused_not_truncated();
    merge_semantics();
    the_real_altair_toml_loads();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
