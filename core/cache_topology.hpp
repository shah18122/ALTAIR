// core/cache_topology.hpp -- how big L1, L2 and L3 actually are, on this box.
//
// Layout decisions are claims about the cache: "this column fits in L1D",
// "this table spills to L3". A claim like that written against a textbook
// number is a guess, and the three numbers differ by an order of magnitude
// between a laptop and a server. So they are read from the operating system,
// and every benchmark that argues from them prints them first.
//
// The line size is the exception. 64 bytes is the line on every x86-64 part
// this project could plausibly run on, it is already a compile-time constant
// in core/lockfree/spsc_ring.hpp, and alignas() needs a constant expression.
// The runtime probe CHECKS it: if the OS reports a different line size, the
// static layout is wrong for that machine and `line_matches()` says so.

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace altair {

/// The line size every static layout in the tree is written against.
inline constexpr std::size_t kCacheLineBytes = 64;

struct CacheTopology {
    std::size_t line = 0;   ///< bytes; 0 = the OS would not say
    std::size_t l1d = 0;    ///< per core
    std::size_t l2 = 0;     ///< per core on most parts, shared on some
    std::size_t l3 = 0;     ///< shared
    bool from_os = false;   ///< false = these are fallbacks, not measurements

    /// Does the machine agree with the compile-time line size?
    [[nodiscard]] bool line_matches() const noexcept {
        return line == 0 || line == kCacheLineBytes;
    }
};

/// Ask the OS. Returns fallbacks with from_os = false if it will not answer --
/// never silently, because a benchmark reporting "fits in L2" against a
/// fallback is reporting a guess as a measurement.
[[nodiscard]] inline CacheTopology read_cache_topology() {
    CacheTopology t;
#if defined(_WIN32)
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    if (bytes > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
            bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(info.data(), &bytes)) {
            for (const auto& e : info) {
                if (e.Relationship != RelationCache) { continue; }
                const CACHE_DESCRIPTOR& c = e.Cache;
                if (c.Type != CacheData && c.Type != CacheUnified) { continue; }
                if (c.Level == 1 && t.l1d == 0) { t.l1d = c.Size; t.line = c.LineSize; }
                if (c.Level == 2 && t.l2 == 0) { t.l2 = c.Size; }
                if (c.Level == 3 && t.l3 == 0) { t.l3 = c.Size; }
            }
            t.from_os = t.l1d != 0;
        }
    }
#elif defined(__linux__) && defined(_SC_LEVEL1_DCACHE_SIZE)
    const long l1 = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    const long ln = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    const long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
    const long l3 = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (l1 > 0) {
        t.l1d = static_cast<std::size_t>(l1);
        t.line = ln > 0 ? static_cast<std::size_t>(ln) : 0;
        t.l2 = l2 > 0 ? static_cast<std::size_t>(l2) : 0;
        t.l3 = l3 > 0 ? static_cast<std::size_t>(l3) : 0;
        t.from_os = true;
    }
#endif
    if (!t.from_os) {
        // Conservative desktop-class figures, flagged as NOT measured.
        t.line = kCacheLineBytes;
        t.l1d = 32u * 1024u;
        t.l2 = 1024u * 1024u;
        t.l3 = 8u * 1024u * 1024u;
    }
    return t;
}

}  // namespace altair
