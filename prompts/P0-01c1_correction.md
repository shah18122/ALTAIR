# CORRECTION 1 to P0-01

> Paste everything below the line into DeepSeek V4.

---

CORRECTION to P0-01 — `core/types`.

Six of eight review gates passed. Two defects must be fixed. Everything else in
your submission passed review: **do not change anything not named here.**

Verified correct and to be left alone: `mul_overflows` branch logic (all four
sign quadrants checked by hand), `lots_of` floor-toward-negative-infinity,
`round_to_tick` in all three modes including negative prices, the `apply_bps`
tie-rounding on all five specified values, the `price_of` `INT64_MIN / -1`
guard, and the concept-based type-safety probes.

---

## DEFECT 1 — anonymous namespace in a header (ODR violation)

**Where:** `core/types/units.hpp` lines 47–73.

**What is wrong:** `namespace { ... }` gives `mul_overflows` and `finite_ld`
**internal linkage**. They are odr-used by `notional_of`, `qty_of`, and
`apply_bps`, which are `constexpr` and therefore implicitly `inline` with
**external linkage**. An inline function with external linkage that references
an internal-linkage entity has a different definition in every translation
unit. That is an ODR violation — ill-formed, no diagnostic required. It
compiles today and silently gives every TU its own copy of the helpers.

**Why it matters:** `units.hpp` will be included by essentially every file in
Altair. This is the wrong thing to have at the base of the dependency graph,
and no compiler will tell you.

**Fix:** rename the anonymous namespace to `detail`.

```cpp
namespace detail {
    // mul_overflows, and the new helpers from Defect 2
} // namespace detail
```

Update the three call sites to `detail::mul_overflows(...)`. `constexpr` already
gives them the inline linkage they need — do not add `static`, which would
reintroduce the same problem.

---

## DEFECT 2 — `constexpr` functions calling non-`constexpr` `<cmath>`

**Where:** `price_from_rupees` and `notional_from_rupees`, which call
`std::isfinite`, `std::round`, and `std::fabs`.

**What is wrong:** those three are **not `constexpr` in C++23** — P0533R9 landed
in C++26, not C++23. A `constexpr` function for which no invocation can ever be
a constant expression is ill-formed, no diagnostic required. GCC and Clang
accept it via `__builtin_` constant folding; MSVC does not. The declarations
therefore mean different things on your dev box and your prod box.

**Why it matters:** the effective-dated charge tables in P3-09 need to be
constant-folded, and they are built from rupee-denominated rates. This has to
work at compile time on both toolchains.

**Fix:** add these to `namespace detail` and use them instead. You already wrote
the equivalent correctly for `StrongReal::is_finite`, so mirror that.

```cpp
namespace detail {

/// True iff v is neither NaN nor infinite. constexpr-safe; no <cmath>.
constexpr bool is_finite_d(double v) noexcept {
    return v == v
        && v != std::numeric_limits<double>::infinity()
        && v != -std::numeric_limits<double>::infinity();
}

/// |v|. constexpr-safe.
constexpr double abs_d(double v) noexcept { return v < 0.0 ? -v : v; }

/// Round half away from zero.
/// PRECONDITION: v is finite and |v| < 2^63. The caller MUST range-check
/// before calling — static_cast<std::int64_t> outside that range is UB.
constexpr double round_half_away(double v) noexcept {
    const double t = static_cast<double>(static_cast<std::int64_t>(v)); // trunc toward zero
    const double frac = v - t;
    if (frac >=  0.5) { return t + 1.0; }
    if (frac <= -0.5) { return t - 1.0; }
    return t;
}

} // namespace detail
```

Rewrite both conversion functions to this shape. **The range check must come
before the rounding**, not after — that ordering is the whole point:

```cpp
[[nodiscard]] constexpr std::expected<Price, ArithError>
price_from_rupees(double rupees) noexcept {
    if (!detail::is_finite_d(rupees)) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const double scaled = rupees * 100.0;
    if (!detail::is_finite_d(scaled)) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    // Range-check BEFORE truncating. 2^63 as a double is exactly
    // 9223372036854775808.0; anything at or beyond it is out of int64 range.
    constexpr double kPow63 = 9223372036854775808.0;
    if (scaled >= kPow63 || scaled < -kPow63) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const double rounded = detail::round_half_away(scaled);
    if (detail::abs_d(scaled - rounded) > 1e-9) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    return Price{static_cast<std::int64_t>(rounded)};
}
```

`notional_from_rupees` is identical but returns `Notional`.

Remove `<cmath>` from `units.hpp`'s include list — it is no longer needed there.
The test file may keep it for `std::nan`.

---

## NEW ASSERTIONS

Add these to `core/types/tests/test_units.cpp`. Do not remove or alter any
existing assertion.

At namespace scope, beside the other `static_assert`s — these are what prove
Defect 2 is actually fixed rather than merely compiling:

```cpp
static_assert(altair::price_from_rupees(25000.50).value() == altair::Price{2500050},
              "price_from_rupees must be usable in a constant expression");
static_assert(!altair::price_from_rupees(0.005).has_value(),
              "half a paise is not representable, at compile time");
static_assert(altair::notional_from_rupees(-100.25).value() == altair::Notional{-10025},
              "notional_from_rupees must be usable in a constant expression");
```

Inside `test_units_rupee_conversion()`, append:

```cpp
check(price_from_rupees(-25000.50).value() == Price{-2500050},
      "price_from_rupees negative");
check(price_from_rupees(1e300).error() == ArithError::NotRepresentable,
      "out of int64 range rejected before truncation");
check(price_from_rupees(-1e300).error() == ArithError::NotRepresentable,
      "negative out of range rejected before truncation");
check(notional_from_rupees(0.0).value() == Notional{0},
      "zero rupees");
```

---

## ONE QUESTION TO ANSWER, NOT TO ACT ON

You stated that MSVC hard-errors on `static_assert(!requires(...))` at namespace
scope. Your concept-based workaround is semantically equivalent and is
**accepted** — keep it. But state in your response the exact MSVC version and
error code you observed, or say plainly that you did not observe it and were
reasoning from general knowledge. Unverified compiler claims compound across 94
cards; I need to know which kind this was.

---

## SCOPE

Change **only**:
- `core/types/units.hpp` — the `detail` namespace rename, the three new helpers,
  the two rewritten conversion functions, and the `<cmath>` include removal.
- `core/types/tests/test_units.cpp` — the new assertions above.

Do not touch `core/types/CMakeLists.txt`. Do not create any file. Do not alter
any other function.

Return both files in full, then your ASSUMPTIONS section and the answer to the
MSVC question.
