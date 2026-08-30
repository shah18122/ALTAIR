# P0-01 — `core/types`: dimensional units and exact money

> Phase 0 · Card 1 of 14 · Status: TODO
> Paste everything below the line into DeepSeek V4 as a single prompt.

---

## 1. CONTEXT

You are implementing the foundation type layer of Altair, a C++23 low-latency
trading engine for Indian equity markets (NSE/BSE).

Every monetary and quantity value in this system is a **distinct compile-time
type**, not a `double`. The purpose is to make an entire class of bug
*impossible to express*: multiplying a price by a number of contracts without
first converting contracts to units via the instrument's lot size. In the
predecessor system that bug silently scaled P&L by a wrong constant.

All money is stored as **integer paise** (1 rupee = 100 paise). NSE tick sizes
are exact multiples of 5 paise, so integer arithmetic is exact and no floating
point comparison ever occurs in the ledger or the order book.

This is a header-only module with no dependencies beyond the C++23 standard library.

---

## 2. FILE MANIFEST

Create exactly these three files. Nothing else.

```
core/types/units.hpp
core/types/CMakeLists.txt
core/types/tests/test_units.cpp
```

---

## 3. INTERFACE CONTRACT

Implement **exactly** these declarations in `core/types/units.hpp`, in this
order, inside `namespace altair`. Do not add, remove, rename, or re-order any
public entity. Bodies are yours; signatures are not.

```cpp
#pragma once

#include <cstdint>
#include <compare>
#include <expected>
#include <type_traits>

// Hot-path marker. Functions carrying this must never allocate.
#if defined(__GNUC__) || defined(__clang__)
#  define ALTAIR_HOT [[gnu::hot]]
#else
#  define ALTAIR_HOT
#endif

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Errors
// ─────────────────────────────────────────────────────────────────────────
enum class ArithError : std::uint8_t {
    Overflow,
    DivideByZero,
    NotRepresentable   // e.g. a rupee value that is not a whole number of paise
};

enum class RoundMode : std::uint8_t { Nearest, Down, Up };

// ─────────────────────────────────────────────────────────────────────────
// Dimension tags — incomplete types, never instantiated
// ─────────────────────────────────────────────────────────────────────────
struct PriceTag;
struct QtyTag;
struct LotsTag;
struct LotSizeTag;
struct NotionalTag;

struct RateTag;
struct YearsTag;
struct VolTag;
struct BpsTag;

// ─────────────────────────────────────────────────────────────────────────
// StrongInt — an exact integral quantity carrying a dimension
// ─────────────────────────────────────────────────────────────────────────
template <typename Tag, typename Rep>
class StrongInt {
public:
    using rep_type = Rep;
    using tag_type = Tag;

    constexpr StrongInt() noexcept = default;
    constexpr explicit StrongInt(Rep v) noexcept;

    [[nodiscard]] constexpr Rep raw() const noexcept;

    friend constexpr auto operator<=>(StrongInt, StrongInt) noexcept = default;
    friend constexpr bool operator==(StrongInt, StrongInt) noexcept = default;

    constexpr StrongInt& operator+=(StrongInt other) noexcept;
    constexpr StrongInt& operator-=(StrongInt other) noexcept;

    [[nodiscard]] constexpr StrongInt operator-() const noexcept;

    friend constexpr StrongInt operator+(StrongInt a, StrongInt b) noexcept {
        return StrongInt{static_cast<Rep>(a.v_ + b.v_)};
    }
    friend constexpr StrongInt operator-(StrongInt a, StrongInt b) noexcept {
        return StrongInt{static_cast<Rep>(a.v_ - b.v_)};
    }

    // Scaling by a DIMENSIONLESS integer of the same representation.
    friend constexpr StrongInt operator*(StrongInt a, Rep k) noexcept {
        return StrongInt{static_cast<Rep>(a.v_ * k)};
    }
    friend constexpr StrongInt operator*(Rep k, StrongInt a) noexcept {
        return StrongInt{static_cast<Rep>(k * a.v_)};
    }

    [[nodiscard]] constexpr bool is_zero()     const noexcept;
    [[nodiscard]] constexpr bool is_positive() const noexcept;
    [[nodiscard]] constexpr bool is_negative() const noexcept;

    [[nodiscard]] static constexpr StrongInt zero() noexcept;
    [[nodiscard]] static constexpr StrongInt min()  noexcept;
    [[nodiscard]] static constexpr StrongInt max()  noexcept;

private:
    Rep v_{};
};

// Dimensionless ratio of two like quantities.
template <typename Tag, typename Rep>
[[nodiscard]] constexpr std::expected<double, ArithError>
ratio(StrongInt<Tag, Rep> numerator, StrongInt<Tag, Rep> denominator) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// StrongReal — a real-valued quantity carrying a dimension
// ─────────────────────────────────────────────────────────────────────────
template <typename Tag>
class StrongReal {
public:
    using rep_type = double;
    using tag_type = Tag;

    constexpr StrongReal() noexcept = default;
    constexpr explicit StrongReal(double v) noexcept;

    [[nodiscard]] constexpr double raw() const noexcept;

    friend constexpr auto operator<=>(StrongReal, StrongReal) noexcept = default;
    friend constexpr bool operator==(StrongReal, StrongReal) noexcept = default;

    constexpr StrongReal& operator+=(StrongReal other) noexcept;
    constexpr StrongReal& operator-=(StrongReal other) noexcept;

    [[nodiscard]] constexpr StrongReal operator-() const noexcept;

    friend constexpr StrongReal operator+(StrongReal a, StrongReal b) noexcept {
        return StrongReal{a.v_ + b.v_};
    }
    friend constexpr StrongReal operator-(StrongReal a, StrongReal b) noexcept {
        return StrongReal{a.v_ - b.v_};
    }
    friend constexpr StrongReal operator*(StrongReal a, double k) noexcept {
        return StrongReal{a.v_ * k};
    }
    friend constexpr StrongReal operator*(double k, StrongReal a) noexcept {
        return StrongReal{k * a.v_};
    }

    [[nodiscard]] constexpr bool is_finite() const noexcept;

private:
    double v_{};
};

// ─────────────────────────────────────────────────────────────────────────
// The dimensions
// ─────────────────────────────────────────────────────────────────────────

/// Price of ONE unit, in integer paise. 1 rupee = 100 paise.
using Price    = StrongInt<PriceTag,    std::int64_t>;

/// A number of tradable UNITS (shares, or contracts x lot_size).
using Qty      = StrongInt<QtyTag,      std::int64_t>;

/// A number of CONTRACTS. Meaningless without a LotSize.
using Lots     = StrongInt<LotsTag,     std::int64_t>;

/// Units per contract. Auto-learned daily; never a literal outside tests.
using LotSize  = StrongInt<LotSizeTag,  std::int64_t>;

/// A money amount in integer paise. Price x Qty, P&L, cost, cash.
using Notional = StrongInt<NotionalTag, std::int64_t>;

/// Interest / dividend rate, per annum, CONTINUOUSLY COMPOUNDED.
using Rate     = StrongReal<RateTag>;

/// Time in years, ACT/365 Fixed.
using Years    = StrongReal<YearsTag>;

/// Annualised volatility (sigma), as a fraction. 0.15 == 15%.
using Vol      = StrongReal<VolTag>;

/// Basis points. 1.0 == 1 bp == 1e-4.
using Bps      = StrongReal<BpsTag>;

// ─────────────────────────────────────────────────────────────────────────
// Cross-dimension operations. These are the ONLY legal ways to combine
// different dimensions. All are checked and return std::expected.
// ─────────────────────────────────────────────────────────────────────────

/// Notional = Price x Qty.  Both in paise / units. Detects int64 overflow.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Notional, ArithError>
notional_of(Price p, Qty q) noexcept;

/// Qty = Lots x LotSize.  The ONLY way to turn contracts into units.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Qty, ArithError>
qty_of(Lots l, LotSize ls) noexcept;

/// Lots = floor(Qty / LotSize).  Floor toward negative infinity, not toward zero.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Lots, ArithError>
lots_of(Qty q, LotSize ls) noexcept;

/// Price = Notional / Qty.  Truncates toward zero. DivideByZero if q == 0.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Price, ArithError>
price_of(Notional n, Qty q) noexcept;

/// Apply a basis-point rate to a money amount. Rounds half away from zero.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Notional, ArithError>
apply_bps(Notional n, Bps b) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// Tick rounding — snap a price to a legal exchange tick
// ─────────────────────────────────────────────────────────────────────────

/// Round `p` to a multiple of `tick`. Nearest uses half away from zero.
/// Down/Up are floor/ceil toward -inf/+inf respectively, INCLUDING for
/// negative prices (spreads can be negative).
/// DivideByZero if tick is zero; NotRepresentable if tick is negative.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Price, ArithError>
round_to_tick(Price p, Price tick, RoundMode mode) noexcept;

/// True iff p is an exact multiple of tick.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<bool, ArithError>
is_on_tick(Price p, Price tick) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// Rupee conversion — for display, analytics, and test readability ONLY.
// Never use doubles in the ledger or the order book.
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr double to_rupees(Price p)    noexcept;
[[nodiscard]] constexpr double to_rupees(Notional n) noexcept;

/// Convert rupees to exact paise. NotRepresentable if the value is not within
/// 1e-9 of a whole number of paise, or is not finite.
[[nodiscard]] constexpr std::expected<Price, ArithError>
price_from_rupees(double rupees) noexcept;

[[nodiscard]] constexpr std::expected<Notional, ArithError>
notional_from_rupees(double rupees) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// Literals — for tests and configuration only
// ─────────────────────────────────────────────────────────────────────────
namespace literals {

[[nodiscard]] constexpr Price    operator""_paise   (unsigned long long v) noexcept;
[[nodiscard]] constexpr Qty      operator""_qty     (unsigned long long v) noexcept;
[[nodiscard]] constexpr Lots     operator""_lots    (unsigned long long v) noexcept;
[[nodiscard]] constexpr LotSize  operator""_lotsize (unsigned long long v) noexcept;
[[nodiscard]] constexpr Notional operator""_np      (unsigned long long v) noexcept; // notional paise
[[nodiscard]] constexpr Bps      operator""_bps     (long double v)        noexcept;
[[nodiscard]] constexpr Vol      operator""_vol     (long double v)        noexcept;
[[nodiscard]] constexpr Years    operator""_years   (long double v)        noexcept;

} // namespace literals
} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `StrongInt` and `StrongReal` are trivially copyable, trivially destructible,
   and the same size as their representation. `sizeof(Price) == 8`.
2. The single-argument constructors are `explicit`. `Price p = 5;` must not compile.
3. **No implicit conversion exists between any two dimensions, or between a
   dimension and its representation type.** `Price * Lots`, `Qty * Qty`,
   `Price + Qty`, and `Price + 5` must all be ill-formed.
4. `notional_of` computes `p.raw() * q.raw()` and returns `ArithError::Overflow`
   if the mathematical product is outside `int64_t`. Perform the check *before*
   any signed overflow occurs — use `__int128` where available, otherwise a
   pre-multiplication magnitude check. Never rely on wraparound.
5. `qty_of` overflows the same way and returns `Overflow`. It returns
   `NotRepresentable` if `ls` is negative, and `DivideByZero` is not applicable.
6. `lots_of` returns `DivideByZero` if `ls.raw() == 0`, `NotRepresentable` if
   `ls` is negative, and otherwise floors toward negative infinity.
   `lots_of(Qty{-10}, LotSize{3})` is `Lots{-4}`, **not** `Lots{-3}`.
7. `price_of` returns `DivideByZero` if `q.raw() == 0`, otherwise truncates
   toward zero (C++ integer division semantics).
8. `apply_bps` computes `n.raw() * b.raw() * 1e-4` and rounds **half away from
   zero** to an integer paise. Returns `Overflow` if the result is outside
   `int64_t` or if `b` is not finite. Do the multiply in `long double` or
   `__int128` scaled arithmetic; a naive `double` multiply on a large notional
   loses paise and is a review failure.
9. `round_to_tick` returns `DivideByZero` if `tick.raw() == 0` and
   `NotRepresentable` if `tick.raw() < 0`.
   - `Nearest`: half away from zero.
   - `Down`: largest multiple of tick that is `<= p` (true floor, so
     `-10007` with tick `5` gives `-10010`).
   - `Up`: smallest multiple of tick that is `>= p` (true ceiling, so
     `-10007` with tick `5` gives `-10005`).
10. `to_rupees` returns `raw() / 100.0`. It is exact for all magnitudes below
    2^53 paise.
11. `price_from_rupees` multiplies by 100, rounds half away from zero, and
    returns `NotRepresentable` if `|rupees*100 - round(rupees*100)| > 1e-9`,
    if the input is NaN or infinite, or if the result is outside `int64_t`.
12. All cross-dimension functions and `round_to_tick` are `constexpr` and usable
    in a `static_assert`.
13. Every public entity carries a doc comment stating its **units** and its
    **preconditions**. A comment that does not name the unit fails review.

---

## 5. CONSTRAINTS

- C++23. Standard library only. No Boost, no third-party headers.
- Header-only. `units.hpp` has no `.cpp`.
- **`units.hpp` may `#include` only these six:** `<cstdint>`, `<compare>`,
  `<expected>`, `<type_traits>`, `<limits>`, `<cmath>`. Nothing else.
  (The test file may additionally include `<cstdio>` for its reporting helper.)
- No exceptions anywhere. No `throw`, no `std::abort`, no assertions that
  terminate in release.
- No dynamic allocation anywhere in this file.
- No `iostream`, no printing, no logging.
- No `using namespace` at file scope.
- Compiles clean at `/W4` (MSVC) and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast` (GCC/Clang). `-Wconversion` will bite:
  use explicit `static_cast` at every narrowing point.
- `core/types/CMakeLists.txt` must define an INTERFACE library named
  `altair_types`, export its include directory as
  `${CMAKE_CURRENT_SOURCE_DIR}/..` so headers resolve as
  `#include <types/units.hpp>`, link `altair_flags`, and register the test
  executable `altair_types_test` from `tests/test_units.cpp` with
  `add_test(NAME types COMMAND altair_types_test)`. The test target links
  `altair_types` and `altair_flags` only — **Catch2 is not available yet**, so
  the test file is a plain `int main()` returning non-zero on failure.

---

## 6. ACCEPTANCE TESTS

Write these in `core/types/tests/test_units.cpp` as a plain `main()` with a
`check(bool, const char*)` helper that counts failures, prints one line per
check, and returns `0` only if every check passed. Use exactly these test
function names.

```cpp
void test_units_traits();
void test_units_construction_and_raw();
void test_units_arithmetic_same_dimension();
void test_units_cross_dimension_products();
void test_units_lots_qty_roundtrip();
void test_units_floor_semantics_negative();
void test_units_overflow_detected();
void test_units_divide_by_zero();
void test_units_apply_bps();
void test_units_rupee_conversion();
void test_units_round_to_tick();
void test_units_type_safety_static_asserts();
```

Required assertions, with the exact expected values:

**test_units_traits**
```
sizeof(Price) == 8
std::is_trivially_copyable_v<Price>
std::is_trivially_destructible_v<Price>
std::is_standard_layout_v<Price>
Price::zero().raw() == 0
```

**test_units_construction_and_raw**
```
Price{2500000}.raw() == 2500000        // NIFTY at 25,000.00
Qty{75}.raw() == 75
(-Price{500}).raw() == -500
Price{0}.is_zero()
Price{1}.is_positive()
Price{-1}.is_negative()
```

**test_units_arithmetic_same_dimension**
```
(Price{100} + Price{50}).raw() == 150
(Price{100} - Price{150}).raw() == -50
(Price{100} * std::int64_t{3}).raw() == 300
(std::int64_t{3} * Price{100}).raw() == 300
Price{100} < Price{200}
Price{100} == Price{100}
ratio(Price{150}, Price{100}).value() == 1.5
!ratio(Price{150}, Price{0}).has_value()
```

**test_units_cross_dimension_products**
```
// NIFTY at 25,000.00 (2,500,000 paise), one lot of 75 units.
notional_of(Price{2500000}, Qty{75}).value() == Notional{187500000}   // Rs 18,75,000
to_rupees(Notional{187500000}) == 1875000.0
price_of(Notional{187500000}, Qty{75}).value() == Price{2500000}
price_of(Notional{100}, Qty{0}).error() == ArithError::DivideByZero
```

**test_units_lots_qty_roundtrip**
```
qty_of(Lots{3}, LotSize{75}).value()  == Qty{225}
lots_of(Qty{225}, LotSize{75}).value() == Lots{3}
lots_of(Qty{230}, LotSize{75}).value() == Lots{3}     // floor, not round
lots_of(Qty{74},  LotSize{75}).value() == Lots{0}
```

**test_units_floor_semantics_negative**
```
lots_of(Qty{-10}, LotSize{3}).value() == Lots{-4}     // floor, NOT -3
lots_of(Qty{-9},  LotSize{3}).value() == Lots{-3}
```

**test_units_overflow_detected**
```
notional_of(Price{Price::max().raw()}, Qty{2}).error() == ArithError::Overflow
qty_of(Lots{Lots::max().raw()}, LotSize{2}).error()    == ArithError::Overflow
notional_of(Price{Price::min().raw()}, Qty{2}).error() == ArithError::Overflow
```

**test_units_divide_by_zero**
```
lots_of(Qty{100}, LotSize{0}).error()  == ArithError::DivideByZero
price_of(Notional{100}, Qty{0}).error() == ArithError::DivideByZero
round_to_tick(Price{100}, Price{0}, RoundMode::Nearest).error() == ArithError::DivideByZero
round_to_tick(Price{100}, Price{-5}, RoundMode::Nearest).error() == ArithError::NotRepresentable
```

**test_units_apply_bps**
```
// STT on equity futures sell from 2026-04-01 is 0.05% = 5 bps.
// Turnover Rs 18,75,000 = 187,500,000 paise.  5 bps = Rs 937.50 = 93,750 paise.
apply_bps(Notional{187500000}, Bps{5.0}).value() == Notional{93750}

// Half away from zero.
apply_bps(Notional{100}, Bps{5.0}).value()  == Notional{1}    // 0.05 -> 0? see note
apply_bps(Notional{1000}, Bps{5.0}).value() == Notional{1}    // 0.5   -> 1
apply_bps(Notional{-1000}, Bps{5.0}).value() == Notional{-1}  // -0.5  -> -1
apply_bps(Notional{3000}, Bps{5.0}).value() == Notional{2}    // 1.5   -> 2
```
> NOTE on the second line: `100 * 5e-4 = 0.05`, which rounds to `0`, not `1`.
> **Assert `Notional{0}` there.** This line is deliberately written wrong in the
> comment above — if you copy it blindly the test will fail. Compute each
> expected value yourself and state in your ASSUMPTIONS section what you
> asserted and why.

**test_units_rupee_conversion**
```
to_rupees(Price{2500050}) == 25000.50
price_from_rupees(25000.50).value() == Price{2500050}
price_from_rupees(0.005).error() == ArithError::NotRepresentable   // half a paise
price_from_rupees(std::nan("")).error() == ArithError::NotRepresentable
```

**test_units_round_to_tick**
```
// NSE tick size is 5 paise for most instruments.
round_to_tick(Price{10007}, Price{5}, RoundMode::Nearest).value() == Price{10005}
round_to_tick(Price{10008}, Price{5}, RoundMode::Nearest).value() == Price{10010}
round_to_tick(Price{10007}, Price{5}, RoundMode::Down).value()    == Price{10005}
round_to_tick(Price{10007}, Price{5}, RoundMode::Up).value()      == Price{10010}

// Negative prices occur on spreads. Down is FLOOR, Up is CEILING.
round_to_tick(Price{-10007}, Price{5}, RoundMode::Down).value() == Price{-10010}
round_to_tick(Price{-10007}, Price{5}, RoundMode::Up).value()   == Price{-10005}

is_on_tick(Price{10005}, Price{5}).value() == true
is_on_tick(Price{10007}, Price{5}).value() == false
```

**test_units_type_safety_static_asserts**
Place these at namespace scope in the test file. They are the most important
assertions in this card — they are what makes the lot-size bug impossible.

```cpp
static_assert(!requires(altair::Price p, altair::Lots l)   { p * l; },
              "Price * Lots must not compile");
static_assert(!requires(altair::Qty a, altair::Qty b)      { a * b; },
              "Qty * Qty must not compile");
static_assert(!requires(altair::Price p, altair::Qty q)    { p + q; },
              "Price + Qty must not compile");
static_assert(!requires(altair::Price p)                   { p + 5; },
              "Price + int must not compile");
static_assert(!requires(altair::Price p)                   { p = 5; },
              "implicit int -> Price assignment must not compile");
static_assert(!std::is_convertible_v<std::int64_t, altair::Price>,
              "int64 must not implicitly convert to Price");
static_assert(!std::is_convertible_v<altair::Price, std::int64_t>,
              "Price must not implicitly convert to int64");
static_assert(!std::is_convertible_v<altair::Qty, altair::Lots>,
              "Qty must not implicitly convert to Lots");

// And the positive cases — these MUST compile:
static_assert(requires(altair::Price p, altair::Qty q)      { altair::notional_of(p, q); });
static_assert(requires(altair::Lots l, altair::LotSize ls)  { altair::qty_of(l, ls); });
static_assert(altair::notional_of(altair::Price{2500000}, altair::Qty{75}).value()
              == altair::Notional{187500000},
              "constexpr evaluation of notional_of");
```

`test_units_type_safety_static_asserts()` itself may have an empty body; the
static_asserts do the work at compile time.

---

## 7. FORBIDDEN

Any of these fails review and the card is returned:

- Adding a file not in the manifest, including a `.cpp` for `units.hpp`.
- Changing any signature in the interface contract.
- Adding an implicit conversion, a non-explicit constructor, or an
  `operator Rep()` on either strong type.
- Adding `operator*` between two different dimensions, or between a strong type
  and a raw type other than its own `Rep`.
- Any `#include` outside the six listed in Constraints.
- Any use of `throw`, `assert`, `iostream`, `printf` outside the test file's
  reporting helper, or dynamic allocation.
- Relying on signed integer overflow to detect overflow. It is undefined
  behaviour and the optimiser will delete your check.
- Using `double` for the `apply_bps` multiply on a large notional. It loses
  paise. Use `__int128` or `long double`.
- Rounding `lots_of` toward zero instead of toward negative infinity.
- Writing a test that asserts whatever your implementation happens to produce.
  The expected values in §6 are the specification; if your code disagrees with
  them, your code is wrong.

---

## 8. RULES

```
RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No `using namespace` at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If a requirement is ambiguous, implement the most conservative reading and
    list the ambiguity under "ASSUMPTIONS" at the end of your response.
```

Return the three files in full, then your ASSUMPTIONS section.
