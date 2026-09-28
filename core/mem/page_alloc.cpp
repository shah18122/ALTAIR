// P0-05a — OS pages with honest hugepage reporting.
//
// All platform code lives here. The header stays free of <windows.h> and
// <sys/mman.h>: dragging windows.h into a widely-included header is how a
// codebase acquires min/max macro damage.

#include <mem/page_alloc.hpp>

#include <cstddef>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define ALTAIR_PAGES 1
#elif defined(__linux__) || defined(__APPLE__)
#  include <sys/mman.h>
#  include <unistd.h>
#  define ALTAIR_PAGES 1
#else
#  define ALTAIR_PAGES 0
#endif

namespace altair {
namespace {

constexpr std::size_t kFallbackPageSize = 4096;

std::size_t query_page_size() noexcept
{
#if defined(_WIN32)
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return si.dwPageSize != 0 ? static_cast<std::size_t>(si.dwPageSize)
                              : kFallbackPageSize;
#elif defined(__linux__) || defined(__APPLE__)
    const long v = ::sysconf(_SC_PAGESIZE);
    return v > 0 ? static_cast<std::size_t>(v) : kFallbackPageSize;
#else
    return kFallbackPageSize;
#endif
}

std::size_t query_huge_page_size() noexcept
{
#if defined(_WIN32)
    return static_cast<std::size_t>(::GetLargePageMinimum());
#elif defined(__linux__)
    // The near-universal default on x86-64. Reading /proc/meminfo would be more
    // precise but would put file I/O in a startup path for no practical gain.
    return static_cast<std::size_t>(2) * 1024 * 1024;
#else
    return 0;
#endif
}

/// Round `n` up to a multiple of `to`. PRECONDITION: to is a power of two > 0.
/// Returns 0 on overflow, which every caller treats as a failed reservation.
constexpr std::size_t round_up(std::size_t n, std::size_t to) noexcept
{
    if (to == 0) {
        return n;
    }
    const std::size_t rem = n % to;
    if (rem == 0) {
        return n;
    }
    const std::size_t add = to - rem;
    if (n > (static_cast<std::size_t>(-1) - add)) {
        return 0;   // would overflow
    }
    return n + add;
}

/// Map `bytes` (already rounded) with or without huge pages.
/// Returns nullptr on failure. No error detail: the caller decides what a
/// failure means, because a refused huge mapping is not an error.
std::byte* os_map(std::size_t bytes, bool huge) noexcept
{
#if defined(_WIN32)
    DWORD flags = MEM_RESERVE | MEM_COMMIT;
    if (huge) {
        flags |= MEM_LARGE_PAGES;
    }
    void* p = ::VirtualAlloc(nullptr, bytes, flags, PAGE_READWRITE);
    return static_cast<std::byte*>(p);
#elif defined(__linux__) || defined(__APPLE__)
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#  if defined(__linux__) && defined(MAP_HUGETLB)
    if (huge) {
        flags |= MAP_HUGETLB;
    }
#  else
    (void)huge;
#  endif
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<std::byte*>(p);
#else
    (void)bytes;
    (void)huge;
    return nullptr;
#endif
}

void os_unmap(std::byte* p, std::size_t bytes) noexcept
{
#if defined(_WIN32)
    (void)bytes;   // MEM_RELEASE requires a size of 0
    ::VirtualFree(p, 0, MEM_RELEASE);
#elif defined(__linux__) || defined(__APPLE__)
    ::munmap(p, bytes);
#else
    (void)p;
    (void)bytes;
#endif
}

} // namespace

std::size_t page_size() noexcept
{
    static const std::size_t v = query_page_size();
    return v;
}

std::size_t huge_page_size() noexcept
{
    static const std::size_t v = query_huge_page_size();
    return v;
}

PageBlock::~PageBlock() noexcept
{
    reset();
}

PageBlock::PageBlock(PageBlock&& other) noexcept
    : data_(other.data_), size_(other.size_), backing_(other.backing_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.backing_ = PageBacking::Default;
}

PageBlock& PageBlock::operator=(PageBlock&& other) noexcept
{
    // Self-move must NOT release the mapping. This is the classic double-free
    // in move-only RAII types.
    if (this == &other) {
        return *this;
    }
    reset();
    data_ = other.data_;
    size_ = other.size_;
    backing_ = other.backing_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.backing_ = PageBacking::Default;
    return *this;
}

std::expected<PageBlock, PageError>
PageBlock::reserve(std::size_t bytes, bool want_huge) noexcept
{
    if (bytes == 0) {
        return std::unexpected(PageError::ZeroSize);
    }
#if !ALTAIR_PAGES
    (void)want_huge;
    return std::unexpected(PageError::Unsupported);
#else
    // Try huge first when asked. A refusal is not an error — fall through.
    if (want_huge) {
        const std::size_t hps = huge_page_size();
        if (hps != 0) {
            const std::size_t hsz = round_up(bytes, hps);
            if (hsz != 0) {
                if (std::byte* p = os_map(hsz, true); p != nullptr) {
                    PageBlock b;
                    b.data_ = p;
                    b.size_ = hsz;
                    b.backing_ = PageBacking::Huge;
                    return b;
                }
            }
        }
        // Refused. Fall back to ordinary pages and report the truth.
    }

    const std::size_t ps = page_size();
    const std::size_t sz = round_up(bytes, ps);
    if (sz == 0) {
        return std::unexpected(PageError::OutOfMemory);
    }
    std::byte* p = os_map(sz, false);
    if (p == nullptr) {
        return std::unexpected(PageError::OutOfMemory);
    }

    PageBlock b;
    b.data_ = p;
    b.size_ = sz;
    b.backing_ = PageBacking::Default;
    return b;
#endif
}

std::byte* PageBlock::data() const noexcept { return data_; }
std::size_t PageBlock::size() const noexcept { return size_; }
PageBacking PageBlock::backing() const noexcept { return backing_; }
bool PageBlock::empty() const noexcept { return data_ == nullptr; }

void PageBlock::reset() noexcept
{
    if (data_ != nullptr) {
        os_unmap(data_, size_);
        data_ = nullptr;
        size_ = 0;
        backing_ = PageBacking::Default;
    }
}

} // namespace altair
