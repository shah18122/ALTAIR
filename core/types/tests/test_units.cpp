// P0-01 acceptance tests for core/types/units.hpp.
// Plain main() (Catch2 not available until P0-03).

#include <types/units.hpp>

#include <cstdio>
#include <cmath>
#include <cstdint>
#include <limits>
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
using namespace altair::literals;

// ─────────────────────────────────────────────────────────────────────────
// Static type-safety assertions. These do the work at compile time.
//
// MSVC hard-errors on a negative requires-expression over these types instead
// of evaluating it to false. Verified on 19.51.36256 x64, /std:c++latest
// /permissive-: C2678 when both operands are class types (Price*Lots, Qty*Qty,
// Price+Qty) and C2679 when the right operand is a builtin (Price+int,
// Price=int). The substitution failure that should be swallowed by the
// requires-expression escapes it instead.
//
// NOT scope-dependent — the same pair of errors fires inside a function body.
// The standard-conforming workaround is to wrap each negative check in a named
// concept, which MSVC does evaluate correctly. Semantics are identical.
// ─────────────────────────────────────────────────────────────────────────
namespace type_safety_probes {

template <typename T, typename U>
concept multipliable = requires(T a, U b) { a * b; };

template <typename T, typename U>
concept addable = requires(T a, U b) { a + b; };

template <typename T, typename U>
concept assignable_from = requires(T a, U b) { a = b; };

} // namespace type_safety_probes

static_assert(!type_safety_probes::multipliable<altair::Price, altair::Lots>,
              "Price * Lots must not compile");
static_assert(!type_safety_probes::multipliable<altair::Qty, altair::Qty>,
              "Qty * Qty must not compile");
static_assert(!type_safety_probes::addable<altair::Price, altair::Qty>,
              "Price + Qty must not compile");
static_assert(!type_safety_probes::addable<altair::Price, int>,
              "Price + int must not compile");
static_assert(!type_safety_probes::assignable_from<altair::Price, int>,
              "implicit int -> Price assignment must not compile");
static_assert(!std::is_convertible_v<std::int64_t, altair::Price>,
              "int64 must not implicitly convert to Price");
static_assert(!std::is_convertible_v<altair::Price, std::int64_t>,
              "Price must not implicitly convert to int64");
static_assert(!std::is_convertible_v<altair::Qty, altair::Lots>,
              "Qty must not implicitly convert to Lots");

static_assert(requires(altair::Price p, altair::Qty q)      { altair::notional_of(p, q); });
static_assert(requires(altair::Lots l, altair::LotSize ls)  { altair::qty_of(l, ls); });
static_assert(altair::notional_of(altair::Price{2500000}, altair::Qty{75}).value()
              == altair::Notional{187500000},
              "constexpr evaluation of notional_of");

static_assert(altair::price_from_rupees(25000.50).value() == altair::Price{2500050},
              "price_from_rupees must be usable in a constant expression");
static_assert(!altair::price_from_rupees(0.005).has_value(),
              "half a paise is not representable, at compile time");
static_assert(altair::notional_from_rupees(-100.25).value() == altair::Notional{-10025},
              "notional_from_rupees must be usable in a constant expression");

void test_units_traits()
{
    check(sizeof(Price) == 8, "sizeof(Price) == 8");
    check(std::is_trivially_copyable_v<Price>, "Price trivially copyable");
    check(std::is_trivially_destructible_v<Price>, "Price trivially destructible");
    check(std::is_standard_layout_v<Price>, "Price standard layout");
    check(Price::zero().raw() == 0, "Price::zero() == 0");
}

void test_units_construction_and_raw()
{
    check(Price{2500000}.raw() == 2500000, "NIFTY at 25,000.00");
    check(Qty{75}.raw() == 75, "Qty{75}");
    check((-Price{500}).raw() == -500, "unary minus");
    check(Price{0}.is_zero(), "Price{0}.is_zero()");
    check(Price{1}.is_positive(), "Price{1}.is_positive()");
    check(Price{-1}.is_negative(), "Price{-1}.is_negative()");
}

void test_units_arithmetic_same_dimension()
{
    check((Price{100} + Price{50}).raw() == 150, "100 + 50 == 150");
    check((Price{100} - Price{150}).raw() == -50, "100 - 150 == -50");
    check((Price{100} * std::int64_t{3}).raw() == 300, "100 * 3 == 300");
    check((std::int64_t{3} * Price{100}).raw() == 300, "3 * 100 == 300");
    check(Price{100} < Price{200}, "100 < 200");
    check(Price{100} == Price{100}, "100 == 100");
    check(ratio(Price{150}, Price{100}).value() == 1.5, "ratio 150/100 == 1.5");
    check(!ratio(Price{150}, Price{0}).has_value(), "ratio by zero has no value");
}

void test_units_cross_dimension_products()
{
    check(notional_of(Price{2500000}, Qty{75}).value() == Notional{187500000},
          "notional_of NIFTY lot");
    check(to_rupees(Notional{187500000}) == 1875000.0, "to_rupees notional");
    check(price_of(Notional{187500000}, Qty{75}).value() == Price{2500000},
          "price_of notional / qty");
    check(price_of(Notional{100}, Qty{0}).error() == ArithError::DivideByZero,
          "price_of by zero");
}

void test_units_lots_qty_roundtrip()
{
    check(qty_of(Lots{3}, LotSize{75}).value() == Qty{225}, "3 lots * 75 == 225");
    check(lots_of(Qty{225}, LotSize{75}).value() == Lots{3}, "225 / 75 == 3 lots");
    check(lots_of(Qty{230}, LotSize{75}).value() == Lots{3}, "230 / 75 floor == 3");
    check(lots_of(Qty{74}, LotSize{75}).value() == Lots{0}, "74 / 75 floor == 0");
}

void test_units_floor_semantics_negative()
{
    check(lots_of(Qty{-10}, LotSize{3}).value() == Lots{-4}, "-10 / 3 floor == -4");
    check(lots_of(Qty{-9}, LotSize{3}).value() == Lots{-3}, "-9 / 3 floor == -3");
}

void test_units_overflow_detected()
{
    check(notional_of(Price{Price::max().raw()}, Qty{2}).error() == ArithError::Overflow,
          "max * 2 overflow");
    check(qty_of(Lots{Lots::max().raw()}, LotSize{2}).error() == ArithError::Overflow,
          "max lots * 2 overflow");
    check(notional_of(Price{Price::min().raw()}, Qty{2}).error() == ArithError::Overflow,
          "min * 2 overflow");
}

void test_units_divide_by_zero()
{
    check(lots_of(Qty{100}, LotSize{0}).error() == ArithError::DivideByZero,
          "lots_of zero lotsize");
    check(price_of(Notional{100}, Qty{0}).error() == ArithError::DivideByZero,
          "price_of zero qty");
    check(round_to_tick(Price{100}, Price{0}, RoundMode::Nearest).error()
              == ArithError::DivideByZero,
          "round_to_tick zero tick");
    check(round_to_tick(Price{100}, Price{-5}, RoundMode::Nearest).error()
              == ArithError::NotRepresentable,
          "round_to_tick negative tick");
}

void test_units_apply_bps()
{
    check(apply_bps(Notional{187500000}, Bps{5.0}).value() == Notional{93750},
          "5 bps on Rs 18,75,000 == Rs 937.50");

    // 100 paise * 5 bp = 0.05 paise -> rounds to 0.
    check(apply_bps(Notional{100}, Bps{5.0}).value() == Notional{0},
          "0.05 paise rounds to 0");
    // 1000 paise * 5 bp = 0.5 paise -> half away from zero -> 1.
    check(apply_bps(Notional{1000}, Bps{5.0}).value() == Notional{1},
          "0.5 paise rounds away to 1");
    check(apply_bps(Notional{-1000}, Bps{5.0}).value() == Notional{-1},
          "-0.5 paise rounds away to -1");
    check(apply_bps(Notional{3000}, Bps{5.0}).value() == Notional{2},
          "1.5 paise rounds away to 2");
}

void test_units_rupee_conversion()
{
    check(to_rupees(Price{2500050}) == 25000.50, "to_rupees price");
    check(price_from_rupees(25000.50).value() == Price{2500050},
          "price_from_rupees 25000.50");
    check(price_from_rupees(0.005).error() == ArithError::NotRepresentable,
          "half paise not representable");
    check(price_from_rupees(std::nan("")).error() == ArithError::NotRepresentable,
          "NaN not representable");
    check(price_from_rupees(-25000.50).value() == Price{-2500050},
          "price_from_rupees negative");
    check(price_from_rupees(1e300).error() == ArithError::NotRepresentable,
          "out of int64 range rejected before truncation");
    check(price_from_rupees(-1e300).error() == ArithError::NotRepresentable,
          "negative out of range rejected before truncation");
    check(notional_from_rupees(0.0).value() == Notional{0},
          "zero rupees");
}

void test_units_round_to_tick()
{
    check(round_to_tick(Price{10007}, Price{5}, RoundMode::Nearest).value()
              == Price{10005}, "10007 nearest 5 -> 10005");
    check(round_to_tick(Price{10008}, Price{5}, RoundMode::Nearest).value()
              == Price{10010}, "10008 nearest 5 -> 10010");
    check(round_to_tick(Price{10007}, Price{5}, RoundMode::Down).value()
              == Price{10005}, "10007 down 5 -> 10005");
    check(round_to_tick(Price{10007}, Price{5}, RoundMode::Up).value()
              == Price{10010}, "10007 up 5 -> 10010");

    check(round_to_tick(Price{-10007}, Price{5}, RoundMode::Down).value()
              == Price{-10010}, "-10007 down 5 -> -10010 (floor)");
    check(round_to_tick(Price{-10007}, Price{5}, RoundMode::Up).value()
              == Price{-10005}, "-10007 up 5 -> -10005 (ceil)");

    check(is_on_tick(Price{10005}, Price{5}).value() == true, "10005 on tick");
    check(is_on_tick(Price{10007}, Price{5}).value() == false, "10007 not on tick");
}

void test_units_type_safety_static_asserts()
{
    // The static_asserts at namespace scope do the work.
}

int main()
{
    std::printf("altair core/types tests\n");
    test_units_traits();
    test_units_construction_and_raw();
    test_units_arithmetic_same_dimension();
    test_units_cross_dimension_products();
    test_units_lots_qty_roundtrip();
    test_units_floor_semantics_negative();
    test_units_overflow_detected();
    test_units_divide_by_zero();
    test_units_apply_bps();
    test_units_rupee_conversion();
    test_units_round_to_tick();
    test_units_type_safety_static_asserts();

    std::printf(failures == 0 ? "PASS\n" : "FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
