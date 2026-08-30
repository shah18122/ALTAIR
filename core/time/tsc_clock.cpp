// P0-03 — invariant-TSC detection, calibration, and drift uncertainty.
//
// This is the only file in core/time permitted to include <chrono>: calibration
// needs a reference clock, and calibration runs once at startup, off the hot
// path. Nothing here leaks a chrono type through the interface.

#include <time/tsc_clock.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#  define ALTAIR_X86 1
#else
#  define ALTAIR_X86 0
#endif

#if ALTAIR_X86
#  if defined(_MSC_VER)
#    include <intrin.h>
#  elif defined(__GNUC__)
#    include <cpuid.h>
#    include <x86intrin.h>
#  endif
#endif

namespace altair {
namespace {

// ── CPUID ────────────────────────────────────────────────────────────────
// Returns false when the leaf is unavailable, leaving regs untouched.
#if ALTAIR_X86
bool cpuid_leaf(std::uint32_t leaf, std::uint32_t regs[4]) noexcept
{
#  if defined(_MSC_VER)
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, static_cast<int>(leaf));
    regs[0] = static_cast<std::uint32_t>(r[0]);
    regs[1] = static_cast<std::uint32_t>(r[1]);
    regs[2] = static_cast<std::uint32_t>(r[2]);
    regs[3] = static_cast<std::uint32_t>(r[3]);
    return true;
#  elif defined(__GNUC__)
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(leaf, &a, &b, &c, &d) == 0) {
        return false;
    }
    regs[0] = a;
    regs[1] = b;
    regs[2] = c;
    regs[3] = d;
    return true;
#  else
    (void)leaf;
    (void)regs;
    return false;
#  endif
}

/// Largest extended CPUID leaf the CPU supports. 0 if extended leaves are absent.
std::uint32_t max_extended_leaf() noexcept
{
    std::uint32_t regs[4] = {0, 0, 0, 0};
    if (!cpuid_leaf(0x8000'0000u, regs)) {
        return 0;
    }
    // EAX < 0x80000000 means extended leaves are not supported at all.
    return regs[0] >= 0x8000'0000u ? regs[0] : 0u;
}
#endif // ALTAIR_X86

/// Detected once, at first use. CPUID costs ~100 ns and must never appear on
/// the hot path, so the answer is cached here and only a bool is read later.
struct CpuFeatures {
    bool invariant_tsc = false;
    bool rdtscp        = false;

    CpuFeatures() noexcept
    {
#if ALTAIR_X86
        const std::uint32_t maxext = max_extended_leaf();
        std::uint32_t regs[4] = {0, 0, 0, 0};

        // Invariant TSC: leaf 0x80000007, EDX bit 8.
        if (maxext >= 0x8000'0007u && cpuid_leaf(0x8000'0007u, regs)) {
            invariant_tsc = (regs[3] & (1u << 8)) != 0u;
        }

        // RDTSCP: leaf 0x80000001, EDX bit 27.
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
        if (maxext >= 0x8000'0001u && cpuid_leaf(0x8000'0001u, regs)) {
            rdtscp = (regs[3] & (1u << 27)) != 0u;
        }
#endif
    }
};

const CpuFeatures& features() noexcept
{
    static const CpuFeatures f{};
    return f;
}

/// Hot-path copy of the RDTSCP bit.
///
/// read_tsc_ordered() must branch on a plain cached bool. A function-local
/// static would make it a guard-variable check on every read, and a
/// namespace-scope `const bool` initialised by dynamic init could be read
/// before that init runs if some other TU's static initialiser reaches the
/// clock first.
///
/// This is zero-initialised before any dynamic initialisation, so an early
/// read yields `false` and takes the LFENCE+RDTSC path — which is always
/// correct, merely slower. There is no ordering in which this is wrong.
bool g_use_rdtscp = false;

const bool g_rdtscp_init = (g_use_rdtscp = features().rdtscp);

// ── steady_clock reference, in nanoseconds ───────────────────────────────
std::uint64_t steady_ns() noexcept
{
    const auto d = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    return static_cast<std::uint64_t>(ns);
}

std::int64_t system_unix_ns() noexcept
{
    const auto d = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

/// steady_clock's own tick period in nanoseconds, at least 1.
double steady_tick_ns() noexcept
{
    using P = std::chrono::steady_clock::period;
    const double ns = (1e9 * static_cast<double>(P::num)) / static_cast<double>(P::den);
    return ns < 1.0 ? 1.0 : ns;
}

constexpr double kQ32Scale = 4294967296.0;   // 2^32

/// Q32.32 encoding of a ns/tick ratio. PRECONDITION: 0 < ns_per_tick < 2^31.
std::uint64_t to_q32(double ns_per_tick) noexcept
{
    const double scaled = ns_per_tick * kQ32Scale;
    if (scaled <= 0.0) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::llround(scaled));
}

/// (a * b) >> 32 with a 128-bit intermediate.
/// A plain 64-bit product overflows: at 3 GHz a 6-hour session is ~6.5e13
/// ticks and the multiplier is ~1.43e9, giving ~9.3e22 against a UINT64_MAX of
/// 1.8e19. UNIT: ticks x (ns/tick << 32) -> ns. PRECONDITION: the true
/// (a*b)>>32 fits in 64 bits, which every realistic session span does.
[[nodiscard]] ALTAIR_HOT std::uint64_t mul_shift32(std::uint64_t a, std::uint64_t b) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    std::uint64_t hi = 0;
    const std::uint64_t lo = _umul128(a, b, &hi);
    return (hi << 32) | (lo >> 32);
#elif defined(__SIZEOF_INT128__)
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(a) * b) >> 32);
#else
    // Portable 64x64 -> 128 by halves.
    //   a*b = hh*2^64 + (lh + hl)*2^32 + ll
    //   (a*b) >> 32 = hh*2^32 + lh + hl + (ll >> 32)
    // Every term is summed with wrapping u64 arithmetic, which yields the
    // correct low 64 bits — and by precondition the result fits in 64 bits.
    const std::uint64_t al = a & 0xFFFF'FFFFull;
    const std::uint64_t ah = a >> 32;
    const std::uint64_t bl = b & 0xFFFF'FFFFull;
    const std::uint64_t bh = b >> 32;
    const std::uint64_t ll = al * bl;
    const std::uint64_t lh = al * bh;
    const std::uint64_t hl = ah * bl;
    const std::uint64_t hh = ah * bh;
    return (hh << 32) + lh + hl + (ll >> 32);
#endif
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// cpu::
// ─────────────────────────────────────────────────────────────────────────
namespace cpu {

bool has_invariant_tsc() noexcept { return features().invariant_tsc; }
bool has_rdtscp() noexcept { return features().rdtscp; }

ALTAIR_HOT std::uint64_t read_tsc_ordered() noexcept
{
#if ALTAIR_X86
    // Branch on a plain cached bool only — never CPUID, never a static guard.
    if (g_use_rdtscp) {
        unsigned int aux = 0;
        return static_cast<std::uint64_t>(__rdtscp(&aux));
    }
    _mm_lfence();
    return static_cast<std::uint64_t>(__rdtsc());
#else
    return steady_ns();
#endif
}

} // namespace cpu

// ─────────────────────────────────────────────────────────────────────────
// TscClock
// ─────────────────────────────────────────────────────────────────────────

TscClock::TscClock(const TscCalibration& cal) noexcept : cal_(cal) {}

std::expected<TscClock, ClockError> TscClock::create() noexcept
{
#if !ALTAIR_X86
    return std::unexpected(ClockError::NotX86);
#else
    if (!cpu::has_invariant_tsc()) {
        // Fail loud. The caller opts in to a degraded clock explicitly.
        return std::unexpected(ClockError::NoInvariantTsc);
    }

    constexpr int kSamples = 5;
    constexpr std::uint64_t kWindowNs = 10'000'000ull;   // 10 ms

    double ratio[kSamples] = {};

    for (int i = 0; i < kSamples; ++i) {
        const std::uint64_t r0 = steady_ns();
        const std::uint64_t t0 = cpu::read_tsc_ordered();

        std::uint64_t r1 = r0;
        while (r1 - r0 < kWindowNs) {
            r1 = steady_ns();
        }
        const std::uint64_t t1 = cpu::read_tsc_ordered();

        const std::uint64_t dt = t1 - t0;
        if (dt == 0) {
            return std::unexpected(ClockError::CalibrationUnstable);
        }
        ratio[i] = static_cast<double>(r1 - r0) / static_cast<double>(dt);
    }

    double mean = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        mean += ratio[i];
    }
    mean /= static_cast<double>(kSamples);

    if (!(mean > 0.0)) {
        return std::unexpected(ClockError::CalibrationUnstable);
    }

    double ss = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const double d = ratio[i] - mean;
        ss += d * d;
    }
    const double sd = std::sqrt(ss / static_cast<double>(kSamples - 1));
    const double stderr_ = sd / std::sqrt(static_cast<double>(kSamples));

    // Relative spread beyond 0.1% means a virtualised or badly contended host.
    if (sd / mean > 1e-3) {
        return std::unexpected(ClockError::CalibrationUnstable);
    }

    TscCalibration cal{};
    cal.ns_per_tick        = mean;
    cal.ns_per_tick_q32    = to_q32(mean);
    // A measurement always has error. If the samples happened to agree exactly,
    // the true uncertainty is still at least one tick of the reference clock.
    cal.ns_per_tick_stderr = stderr_ > 0.0 ? stderr_ : mean * 1e-9;
    cal.samples            = kSamples;
    cal.source             = ClockSource::InvariantTsc;

    // Anchor last, so now() is as close to the system clock as possible.
    cal.anchor_ticks = cpu::read_tsc_ordered();
    cal.anchor       = Timestamp{system_unix_ns()};

    return TscClock{cal};
#endif
}

TscClock TscClock::create_fallback() noexcept
{
    TscCalibration cal{};
    cal.ns_per_tick        = 1.0;                              // ticks ARE ns here
    cal.ns_per_tick_q32    = static_cast<std::uint64_t>(1) << 32;
    cal.ns_per_tick_stderr = steady_tick_ns();                 // never zero
    cal.samples            = 0;
    cal.source             = ClockSource::SteadyFallback;
    cal.anchor_ticks       = steady_ns();
    cal.anchor             = Timestamp{system_unix_ns()};
    return TscClock{cal};
}

ALTAIR_HOT std::uint64_t TscClock::now_ticks() const noexcept
{
    return cal_.source == ClockSource::InvariantTsc ? cpu::read_tsc_ordered()
                                                    : steady_ns();
}

ALTAIR_HOT Duration TscClock::ticks_to_duration(std::uint64_t ticks) const noexcept
{
    return Duration{static_cast<std::int64_t>(
        mul_shift32(ticks, cal_.ns_per_tick_q32))};
}

ALTAIR_HOT Duration TscClock::elapsed_since(std::uint64_t t0_ticks) const noexcept
{
    // Unsigned subtraction first, so a TSC past INT64_MAX still yields the
    // correct span.
    return ticks_to_duration(now_ticks() - t0_ticks);
}

ALTAIR_HOT Timestamp TscClock::now() const noexcept
{
    return cal_.anchor + ticks_to_duration(now_ticks() - cal_.anchor_ticks);
}

Duration TscClock::uncertainty_of(Duration measured) const noexcept
{
    const double span = static_cast<double>(measured.raw());
    const double mag  = span < 0.0 ? -span : span;
    const double rel  = cal_.ns_per_tick > 0.0
                            ? cal_.ns_per_tick_stderr / cal_.ns_per_tick
                            : 0.0;
    // First-order propagation through the multiply, plus one tick of read
    // quantisation. Never zero: a measurement with no error bar is a lie.
    const double sigma = mag * rel + cal_.ns_per_tick;
    const std::int64_t ns = std::llround(sigma);
    return Duration{ns < 1 ? 1 : ns};
}

const TscCalibration& TscClock::calibration() const noexcept { return cal_; }
ClockSource TscClock::source() const noexcept { return cal_.source; }
bool TscClock::is_invariant_tsc() const noexcept {
    return cal_.source == ClockSource::InvariantTsc;
}

} // namespace altair
