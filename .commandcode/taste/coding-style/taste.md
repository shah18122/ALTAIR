# Coding Style Preferences

- Prefers zero-warning C++ builds using `/W4` (MSVC) or `-Wall -Wextra -Wpedantic -Wconversion` (GCC/Clang). Confidence: 0.9
- Prefers no exceptions on the hot path; return `std::expected<T, Error>` instead. Confidence: 0.9
- Prefers no dynamic allocation inside hot-path functions (no `new`, `malloc`, `std::vector` growth, `std::string`, `shared_ptr`, `std::function`). Confidence: 0.9
- Prefers no `using namespace` at file scope in headers. Confidence: 0.9
- Prefers every public function to carry a doc comment stating its units and preconditions. Confidence: 0.9
- Prefers acceptance tests to be written with assertions and expected numeric values specified up front, rather than tests that assert whatever the implementation happens to produce. Confidence: 0.9
- Treats financial/physical correctness (units, sign conventions, day-count basis, boundary cases, no look-ahead, no sampling above Nyquist, conservation invariants) as the highest-priority review gate. Confidence: 0.9
- Prefers strong, dimension-tagged value types for money and quantities (a distinct compile-time type per dimension) so illegal cross-dimension operations are compile errors rather than runtime bugs. Confidence: 0.85
- Prefers exact integer money (integer paise) and integer quantities; floating point only for display/analytics/test readability, never in the ledger or order book. Confidence: 0.85
- Prefers explicit single-argument constructors and no implicit conversions between dimensions or to/from the raw representation type. Confidence: 0.85
- Prefers overflow to be detected before it occurs (pre-multiplication magnitude checks or __int128), never relying on signed wraparound (undefined behaviour). Confidence: 0.85
- Prefers financial rounding conventions: round half away from zero for money, and floor toward negative infinity (not toward zero) for signed integer division of quantities. Confidence: 0.8
- Prefers compile-time type-safety assertions (static_assert / negative requires) as the primary guard for making illegal operations unexpressable. Confidence: 0.8
- Prefers numeric utility functions to be constexpr and usable in static_assert. Confidence: 0.7
- Prefers explicit `static_cast` at every narrowing point rather than relying on implicit conversions (pairing `-Wconversion`/`-Wsign-conversion`). Confidence: 0.7
- Prefers header-only modules to restrict includes to a small, explicit whitelist of standard headers, with any include outside the list a review failure. Confidence: 0.6
- Prefers implementing float-math helpers (isfinite, abs, round-half-away) by hand as constexpr routines instead of pulling in `<cmath>`. Confidence: 0.7
- Prefers range-checking before truncation/static_cast to `std::int64_t` (explicit 2^63 boundary), and asserts there is no truncation-induced UB. Confidence: 0.7
