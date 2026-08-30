// Toolchain smoke test.
// Proves: C++23 is on, the flags target applies, CTest is wired.
// Deliberately dependency-free so it works before vcpkg is fully configured.
// Replaced by Catch2 coverage as the Phase 0 cards land.

#include <cstdio>
#include <expected>
#include <span>
#include <version>

namespace {

[[nodiscard]] std::expected<int, const char*> divide(int a, int b) noexcept
{
    if (b == 0) { return std::unexpected("division by zero"); }
    return a / b;
}

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) { ++failures; std::printf("  FAIL: %s\n", what); }
    else     { std::printf("  ok  : %s\n", what); }
}

} // namespace

int main()
{
    std::printf("altair smoke test\n");

#if __cplusplus >= 202302L
    check(true, "C++23 mode");
#else
    check(false, "C++23 mode");
#endif

    check(divide(10, 2).value() == 5,        "std::expected happy path");
    check(!divide(1, 0).has_value(),         "std::expected error path");

    constexpr int arr[] = {1, 2, 3, 4};
    const std::span<const int> s{arr};
    check(s.size() == 4,                     "std::span");

    std::printf(failures == 0 ? "PASS\n" : "FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
