// P0-05a acceptance tests for core/mem/page_alloc.hpp.
// Plain main() (Catch2 still unavailable).
//
// This card touches the OS, so the assertions are PROPERTIES that hold on every
// machine — never "the page size is 4096".

#include <mem/page_alloc.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

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

void test_page_size_sane()
{
    const std::size_t ps = page_size();
    const std::size_t hps = huge_page_size();

    check(ps >= 1, "page_size() >= 1");
    check((ps & (ps - 1)) == 0, "page_size() is a power of two");
    check(ps >= 4096, "page_size() >= 4096");
    check(hps == 0 || hps > ps, "huge_page_size() is 0 or larger than a page");
    check(hps == 0 || (hps & (hps - 1)) == 0, "huge_page_size() is 0 or a power of two");

    std::printf("        page_size = %zu, huge_page_size = %zu\n", ps, hps);
}

void test_page_block_reserve_and_size()
{
    const std::size_t ps = page_size();

    auto b = PageBlock::reserve(1, false);
    check(b.has_value(), "reserve(1) succeeds");
    if (b.has_value()) {
        check(b->size() >= ps, "a 1-byte request rounds up to a whole page");
        check(b->size() % ps == 0, "size is a whole number of pages");
        check(!b->empty(), "block is not empty");
        check(b->data() != nullptr, "data() is non-null");
    }

    auto big = PageBlock::reserve(ps * 4 + 1, false);
    check(big.has_value(), "reserve(4 pages + 1) succeeds");
    if (big.has_value()) {
        check(big->size() >= ps * 5, "the trailing +1 rounds up a whole extra page");
        check(big->size() % ps == 0, "size is a whole number of pages");
    }
}

void test_page_block_writable()
{
    auto b = PageBlock::reserve(page_size() * 2, false);
    check(b.has_value(), "reserve(2 pages) succeeds");
    if (!b.has_value()) {
        return;
    }

    // First, middle, and LAST byte. The last one catches an off-by-one in the
    // rounding that a first-byte-only test would miss entirely.
    b->data()[0] = std::byte{0xAB};
    b->data()[b->size() / 2] = std::byte{0xCD};
    b->data()[b->size() - 1] = std::byte{0xEF};

    check(b->data()[0] == std::byte{0xAB}, "first byte round-trips");
    check(b->data()[b->size() / 2] == std::byte{0xCD}, "middle byte round-trips");
    check(b->data()[b->size() - 1] == std::byte{0xEF}, "LAST byte round-trips");
}

void test_page_block_alignment()
{
    auto b = PageBlock::reserve(page_size(), false);
    check(b.has_value(), "reserve for alignment check succeeds");
    if (!b.has_value()) {
        return;
    }
    const auto addr = reinterpret_cast<std::uintptr_t>(b->data());
    check(addr % page_size() == 0, "data() is page-aligned");
    check(addr % 64 == 0, "data() is cache-line aligned");
}

void test_page_block_move_semantics()
{
    check(!std::is_copy_constructible_v<PageBlock>, "PageBlock is not copy constructible");
    check(!std::is_copy_assignable_v<PageBlock>,    "PageBlock is not copy assignable");
    check(std::is_move_constructible_v<PageBlock>,  "PageBlock is move constructible");
    check(std::is_move_assignable_v<PageBlock>,     "PageBlock is move assignable");

    auto a = PageBlock::reserve(page_size(), false);
    check(a.has_value(), "reserve for move check succeeds");
    if (!a.has_value()) {
        return;
    }
    std::byte* p = a->data();

    PageBlock moved{std::move(*a)};
    check(moved.data() == p, "move ctor transfers the mapping");
    check(a->empty() && a->data() == nullptr && a->size() == 0,
          "move ctor empties the source");

    auto x = PageBlock::reserve(page_size(), false);
    auto y = PageBlock::reserve(page_size(), false);
    check(x.has_value() && y.has_value(), "reserve two for move-assign check");
    if (!x.has_value() || !y.has_value()) {
        return;
    }
    *x = std::move(*y);
    check(x->data() != nullptr, "move assign leaves the target mapped");
    check(y->empty(), "move assign empties the source");

    // Self-move must not release. The classic move-only double-free.
    PageBlock& ref = *x;
    ref = std::move(ref);
    check(x->data() != nullptr && !x->empty(), "self-move does not release");
    x->data()[0] = std::byte{0x11};
    check(x->data()[0] == std::byte{0x11}, "still mapped and writable after self-move");
}

void test_page_block_huge_request_reports_truth()
{
    constexpr std::size_t k4Mib = 4u * 1024u * 1024u;

    auto h = PageBlock::reserve(k4Mib, true);
    check(h.has_value(), "a huge request NEVER fails just because huge was refused");
    if (!h.has_value()) {
        return;
    }
    check(h->size() >= k4Mib, "huge request delivers at least what was asked");
    check(h->data() != nullptr, "huge request data() is non-null");
    h->data()[h->size() - 1] = std::byte{0x5A};
    check(h->data()[h->size() - 1] == std::byte{0x5A}, "usable either way");

    if (h->backing() == PageBacking::Huge) {
        const std::size_t hps = huge_page_size();
        check(hps > 0, "Huge backing implies a non-zero huge page size");
        check(h->size() % hps == 0, "huge block is a whole number of huge pages");
        check(reinterpret_cast<std::uintptr_t>(h->data()) % hps == 0,
              "huge block is huge-page aligned");
        std::printf("        huge pages GRANTED (%zu-byte pages)\n", hps);
    } else {
        check(h->size() % page_size() == 0, "fallback block is a whole number of pages");
        std::printf("        huge pages REFUSED — fell back to default, reported honestly\n");
    }

    auto d = PageBlock::reserve(k4Mib, false);
    check(d.has_value(), "non-huge reserve succeeds");
    if (d.has_value()) {
        check(d->backing() == PageBacking::Default,
              "want_huge == false never reports Huge");
    }
}

void test_page_block_error_paths()
{
    check(PageBlock::reserve(0, false).error() == PageError::ZeroSize,
          "reserve(0, false) is ZeroSize");
    check(PageBlock::reserve(0, true).error() == PageError::ZeroSize,
          "reserve(0, true) is ZeroSize");

    PageBlock e;
    check(e.empty() && e.data() == nullptr && e.size() == 0,
          "default-constructed block is empty");
    e.reset();
    check(e.empty(), "reset() on an empty block is a no-op");
}

void test_page_block_raii_release()
{
    // Reserve and drop in a loop. A leaking destructor exhausts the address
    // space long before this finishes; a double-free crashes. Reaching the
    // line after the loop is the assertion.
    constexpr std::size_t k4Mib = 4u * 1024u * 1024u;
    bool all_ok = true;
    for (int i = 0; i < 200; ++i) {
        auto b = PageBlock::reserve(k4Mib, false);
        if (!b.has_value()) { all_ok = false; break; }
        b->data()[0] = std::byte{1};
    }
    check(all_ok, "200 x 4 MiB reserve/destruct cycles all succeeded");
    check(true, "survived 800 MiB of churn — no leak, no double-free");

    auto b2 = PageBlock::reserve(page_size(), false);
    check(b2.has_value(), "reserve for explicit reset check");
    if (!b2.has_value()) {
        return;
    }
    b2->reset();
    check(b2->empty(), "reset() releases");
    b2->reset();
    check(b2->empty(), "reset() is idempotent");
}

int main()
{
    std::printf("altair core/mem page_alloc tests\n");
    test_page_size_sane();
    test_page_block_reserve_and_size();
    test_page_block_writable();
    test_page_block_alignment();
    test_page_block_move_semantics();
    test_page_block_huge_request_reports_truth();
    test_page_block_error_paths();
    test_page_block_raii_release();

    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
