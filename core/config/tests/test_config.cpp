// P0-08a acceptance tests for core/config/config.hpp and core/config/store.hpp.
// Plain main() (Catch2 still unavailable — vcpkg is not installed on this box).

#include <config/config.hpp>
#include <config/store.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

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

void test_config_set_and_get()
{
    ConfigSnapshot c;
    check(c.size() == 0, "a fresh snapshot is empty");
    check(c.version() == 0, "and version 0");

    check(c.set_int("risk.max_lots", 40).has_value(), "set_int");
    check(c.set_real("risk.kelly_fraction", 0.25).has_value(), "set_real");
    check(c.set_bool("strategy.enabled", true).has_value(), "set_bool");
    check(c.size() == 3, "size == 3");

    check(c.get_int(c.find("risk.max_lots").value()).value() == 40, "int round-trips");
    check(c.get_real(c.find("risk.kelly_fraction").value()).value() == 0.25,
          "real round-trips");
    check(c.get_bool(c.find("strategy.enabled").value()).value() == true,
          "bool round-trips");

    // Exact at the extremes — bit_cast, never a pointer cast.
    (void)c.set_int("edge.min", std::numeric_limits<std::int64_t>::min());
    check(c.get_int(c.find("edge.min").value()).value()
              == std::numeric_limits<std::int64_t>::min(),
          "INT64_MIN round-trips exactly");
    (void)c.set_real("edge.neg", -1.5e-300);
    check(c.get_real(c.find("edge.neg").value()).value() == -1.5e-300,
          "a tiny negative double round-trips exactly");

    check(c.find("nope").error() == ConfigError::NotFound, "an absent key is NotFound");
}

void test_config_type_safety()
{
    ConfigSnapshot c;
    (void)c.set_int("i", 1);
    (void)c.set_real("r", 1.0);
    (void)c.set_bool("b", true);

    check(c.get_real(c.find("i").value()).error() == ConfigError::WrongType,
          "int read as real is WrongType");
    check(c.get_bool(c.find("i").value()).error() == ConfigError::WrongType,
          "int read as bool is WrongType, NOT 1 -> true");
    check(c.get_int(c.find("r").value()).error() == ConfigError::WrongType,
          "real read as int is WrongType");
    check(c.get_int(c.find("b").value()).error() == ConfigError::WrongType,
          "bool read as int is WrongType, NOT true -> 1");

    check(c.type_of(c.find("i").value()) == ConfigType::Int, "type_of Int");
    check(c.type_of(c.find("r").value()) == ConfigType::Real, "type_of Real");
    check(c.type_of(c.find("b").value()) == ConfigType::Bool, "type_of Bool");
    check(c.type_of(ConfigHandle{}) == ConfigType::None, "type_of invalid is None");

    check(c.get_int(ConfigHandle{}).error() == ConfigError::NotFound,
          "a default handle is NotFound, not a read of entry 0");
    check(c.get_int(ConfigHandle{9999}).error() == ConfigError::NotFound,
          "an out-of-range handle is NotFound");
}

void test_config_key_limits_and_capacity()
{
    ConfigSnapshot c;
    check(c.set_int(nullptr, 1).error() == ConfigError::EmptyKey, "null key rejected");
    check(c.set_int("", 1).error() == ConfigError::EmptyKey, "empty key rejected");

    char longest[kMaxConfigKeyLen + 1];
    std::memset(longest, 'k', kMaxConfigKeyLen);
    longest[kMaxConfigKeyLen] = '\0';
    check(c.set_int(longest, 1).has_value(), "a key exactly at the limit is accepted");

    char toolong[kMaxConfigKeyLen + 2];
    std::memset(toolong, 'k', kMaxConfigKeyLen + 1);
    toolong[kMaxConfigKeyLen + 1] = '\0';
    check(c.set_int(toolong, 1).error() == ConfigError::KeyTooLong,
          "one byte over the limit is rejected");
    check(c.size() == 1, "the rejection changed nothing");

    ConfigSnapshot f;
    bool all_set = true;
    char key[16];
    for (std::size_t i = 0; i < kMaxConfigEntries; ++i) {
        std::snprintf(key, sizeof(key), "key_%zu", i);
        if (!f.set_int(key, static_cast<std::int64_t>(i)).has_value()) {
            all_set = false;
        }
    }
    check(all_set, "128 distinct keys all fit");
    check(f.size() == kMaxConfigEntries, "size == kMaxConfigEntries");

    check(f.set_int("one_more", 1).error() == ConfigError::Full,
          "the 129th distinct key is Full");
    check(f.size() == kMaxConfigEntries, "and the rejection changed nothing");

    // Overwriting AT capacity still works — it is not a new entry.
    check(f.set_int("key_0", 999).has_value(), "overwrite at capacity succeeds");
    check(f.size() == kMaxConfigEntries, "size unchanged by an overwrite");
    check(f.get_int(f.find("key_0").value()).value() == 999, "the overwrite took");
}

void test_config_overwrite_semantics()
{
    ConfigSnapshot c;
    (void)c.set_int("x", 1);
    check(c.size() == 1, "one entry");
    (void)c.set_int("x", 2);
    check(c.size() == 1, "overwriting does not add an entry");
    check(c.get_int(c.find("x").value()).value() == 2, "the value updated");

    // Overwriting REPLACES the type.
    (void)c.set_real("x", 3.5);
    check(c.size() == 1, "a type change does not add an entry");
    check(c.type_of(c.find("x").value()) == ConfigType::Real, "the type changed");
    check(c.get_real(c.find("x").value()).value() == 3.5, "the new value reads back");
    check(c.get_int(c.find("x").value()).error() == ConfigError::WrongType,
          "and the old type no longer reads");

    c.clear();
    check(c.size() == 0, "clear empties");
    check(c.find("x").error() == ConfigError::NotFound, "and the key is gone");
}

void test_config_content_hash_order_independent()
{
    ConfigSnapshot a, b;
    (void)a.set_int("alpha", 1);
    (void)a.set_real("beta", 2.5);
    (void)a.set_bool("gamma", true);

    (void)b.set_bool("gamma", true);
    (void)b.set_real("beta", 2.5);
    (void)b.set_int("alpha", 1);

    check(a.content_hash() == b.content_hash(),
          "SAME CONTENT, DIFFERENT ORDER -> same hash (CLAUDE.md rule 10)");

    a.set_version(7);
    b.set_version(99);
    check(a.content_hash() == b.content_hash(),
          "the version is identity, not content");

    ConfigSnapshot d;
    (void)d.set_int("alpha", 1);
    (void)d.set_real("beta", 2.5);
    (void)d.set_bool("gamma", false);
    check(d.content_hash() != a.content_hash(), "one flipped bool moves the hash");

    ConfigSnapshot e;
    (void)e.set_int("alphaX", 1);
    (void)e.set_real("beta", 2.5);
    (void)e.set_bool("gamma", true);
    check(e.content_hash() != a.content_hash(), "one renamed key moves the hash");

    ConfigSnapshot z1, z2;
    check(z1.content_hash() == z2.content_hash(), "empty snapshots hash alike");
    check(z1.content_hash() != a.content_hash(), "and differ from a populated one");

    // Type is part of the content: same key, same bits, different type.
    ConfigSnapshot t1, t2;
    (void)t1.set_int("k", 1);
    (void)t2.set_bool("k", true);
    check(t1.content_hash() != t2.content_hash(),
          "same key and bits, different type -> different hash");
}

void test_config_snapshot_traits()
{
    check(std::is_trivially_copyable_v<ConfigSnapshot>,
          "ConfigSnapshot is trivially copyable — the seqlock requires it");
    check(std::is_trivially_copyable_v<ConfigEntry>, "ConfigEntry is trivially copyable");
    check(kMaxConfigEntries == 128, "kMaxConfigEntries == 128");
    check(kMaxConfigKeyLen == 47, "kMaxConfigKeyLen == 47");

    std::printf("        sizeof(ConfigEntry) = %zu, sizeof(ConfigSnapshot) = %zu\n",
                sizeof(ConfigEntry), sizeof(ConfigSnapshot));
}

void test_config_store_publish_and_refresh()
{
    ConfigStore store;
    check(store.version() == 0, "nothing published yet");
    check(store.publishes() == 0, "publish count 0");

    ConfigSnapshot local;
    check(!store.refresh(local), "refresh against an empty store is false");
    check(local.size() == 0, "and copies nothing");

    ConfigSnapshot s;
    (void)s.set_int("a", 1);
    s.set_version(1);
    store.publish(s);
    check(store.version() == 1, "version published");
    check(store.publishes() == 1, "publish counted");

    check(store.refresh(local), "the version moved, so refresh copies");
    check(local.version() == 1, "local version updated");
    check(local.get_int(local.find("a").value()).value() == 1, "value visible");

    check(!store.refresh(local), "a second refresh is a no-op");
    check(local.version() == 1, "and leaves the version alone");

    ConfigSnapshot s2;
    (void)s2.set_int("a", 2);
    (void)s2.set_int("b", 3);
    s2.set_version(2);
    store.publish(s2);
    check(store.refresh(local), "a second publish is picked up");
    check(local.version() == 2, "version 2");
    check(local.size() == 2, "two entries now");
    check(local.get_int(local.find("a").value()).value() == 2, "the old value is gone");
    check(local.content_hash() == s2.content_hash(), "content_hash travels intact");
}

void test_config_store_concurrent_readers()
{
    // Same construction as the P0-06b seqlock test, for the same reason:
    // terminate on the EVIDENCE, not on a timer, or the test can pass having
    // observed almost nothing.
    constexpr std::uint64_t kTargetChanges = 1'000;
    constexpr std::uint64_t kMaxAttempts = 50'000'000;

    static ConfigStore store;
    std::atomic<int> readers_done{0};
    std::atomic<std::uint64_t> inconsistent{0};
    std::atomic<std::uint64_t> bad_version{0};
    std::atomic<std::uint64_t> refreshes{0};
    std::atomic<std::uint64_t> published{0};
    std::atomic<bool> gave_up{false};
    std::atomic<bool> writer_ready{false};

    auto build = [](std::uint64_t n) {
        ConfigSnapshot s;
        (void)s.set_int("n", static_cast<std::int64_t>(n));
        (void)s.set_int("double_n", static_cast<std::int64_t>(2 * n));
        (void)s.set_bool("flag", (n % 2) == 0);
        s.set_version(n);
        return s;
    };

    std::thread writer([&] {
        std::uint64_t n = 1;
        store.publish(build(n));
        writer_ready.store(true, std::memory_order_release);
        ++n;
        while (readers_done.load(std::memory_order_acquire) < 2) {
            store.publish(build(n));
            ++n;
        }
        published.store(n - 1, std::memory_order_relaxed);
    });

    auto reader = [&] {
        while (!writer_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        ConfigSnapshot local;
        std::uint64_t changes = 0, attempts = 0;
        std::uint64_t local_bad = 0, local_badver = 0;

        while (changes < kTargetChanges) {
            if (++attempts > kMaxAttempts) {
                gave_up.store(true, std::memory_order_relaxed);
                break;
            }
            if (!store.refresh(local)) {
                // YIELD, do not hot-spin. An unchanged refresh() is 0.63 ns
                // while a publish copies a 9 KB snapshot, so a spinning reader
                // burns the whole attempt cap before a loaded writer can
                // produce kTargetChanges versions — 4 failures in 6 loaded
                // passes. Yielding makes an "attempt" a real scheduling
                // opportunity instead of a spin iteration, which is also what
                // an actual reader would do.
                std::this_thread::yield();
                continue;
            }
            ++changes;

            const auto hn = local.find("n");
            const auto hd = local.find("double_n");
            const auto hf = local.find("flag");
            if (!hn.has_value() || !hd.has_value() || !hf.has_value()) {
                ++local_bad;
                continue;
            }
            const auto n = local.get_int(hn.value()).value_or(-1);
            const auto d = local.get_int(hd.value()).value_or(-1);
            const auto f = local.get_bool(hf.value()).value_or(false);

            if (d != 2 * n || f != ((n % 2) == 0)) {
                ++local_bad;
            }
            if (static_cast<std::uint64_t>(n) != local.version()) {
                ++local_badver;
            }
        }
        inconsistent.fetch_add(local_bad, std::memory_order_relaxed);
        bad_version.fetch_add(local_badver, std::memory_order_relaxed);
        refreshes.fetch_add(changes, std::memory_order_relaxed);
        readers_done.fetch_add(1, std::memory_order_release);
    };

    std::thread r1(reader);
    std::thread r2(reader);
    r1.join();
    r2.join();
    writer.join();

    check(inconsistent.load() == 0,
          "NO inconsistent snapshot was ever observed across 2 readers");
    check(bad_version.load() == 0,
          "every observed snapshot's payload matched its own version");
    check(!gave_up.load(), "neither reader hit the attempt cap");
    check(refreshes.load() >= 2 * kTargetChanges,
          "at least 2'000 distinct versions observed — the evidence floor");

    std::printf("        %llu versions published, %llu refreshes observed\n",
                static_cast<unsigned long long>(published.load()),
                static_cast<unsigned long long>(refreshes.load()));
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");
    constexpr int kOps = 1'000'000;

    ConfigStore store;
    ConfigSnapshot s;
    char key[16];
    for (std::size_t i = 0; i < kMaxConfigEntries; ++i) {
        std::snprintf(key, sizeof(key), "key_%zu", i);
        (void)s.set_int(key, static_cast<std::int64_t>(i));
    }
    s.set_version(1);
    store.publish(s);

    ConfigSnapshot local;
    (void)store.refresh(local);
    std::uint64_t sink = 0;

    // The steady-state hot path: version unchanged, so no copy at all.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        sink += store.refresh(local) ? 1u : 0u;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns0 = std::chrono::duration<double, std::nano>(t1 - t0).count()
                     / static_cast<double>(kOps);
    std::printf("  refresh(), unchanged          %6.2f ns/call (budget < 5)   %s\n",
                ns0, ns0 < 5.0 ? "OK" : "OVER");

    // Worst-case key lookup: the last of 128 entries. This is why handles exist.
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        const auto h = local.find("key_127");
        sink += h.has_value() ? h.value().index() : 0u;
    }
    const auto t3 = std::chrono::steady_clock::now();
    const double ns1 = std::chrono::duration<double, std::nano>(t3 - t2).count()
                     / static_cast<double>(kOps);
    std::printf("  find(\"key_127\"), 128 entries   %6.2f ns/call (no budget)\n", ns1);

    const ConfigHandle h = local.find("key_127").value();
    const auto t4 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        sink += static_cast<std::uint64_t>(local.get_int(h).value_or(0));
    }
    const auto t5 = std::chrono::steady_clock::now();
    const double ns2 = std::chrono::duration<double, std::nano>(t5 - t4).count()
                     / static_cast<double>(kOps);
    std::printf("  get_int(handle)               %6.2f ns/call (budget < 5)   %s\n",
                ns2, ns2 < 5.0 ? "OK" : "OVER");
    std::printf("    handle is %.0fx cheaper than the scan — resolve once at startup\n",
                ns1 / (ns2 > 0.01 ? ns2 : 0.01));

    if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf("  (unreachable)\n"); }
}

} // namespace

int main()
{
    std::printf("altair core/config store tests\n");
    test_config_set_and_get();
    test_config_type_safety();
    test_config_key_limits_and_capacity();
    test_config_overwrite_semantics();
    test_config_content_hash_order_independent();
    test_config_snapshot_traits();
    test_config_store_publish_and_refresh();
    test_config_store_concurrent_readers();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
