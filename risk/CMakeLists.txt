# risk/ - sizing, limits, portfolio greeks, and the COST CALCULATOR.
#
# P3-09 is the cost calculator: CLAUDE.md rule 5 says every signal is priced
# net of full cost before it exists, and this is the file that makes that
# enforceable.

add_library(altair_risk INTERFACE)
add_library(altair::risk ALIAS altair_risk)

target_include_directories(altair_risk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_risk INTERFACE
    altair_analytics
    altair_types
    altair_time
    altair_instruments
    altair_flags
)

# P3-09b. The charges.toml reader, in a SEPARATE compiled target for the same
# reason core/config's TOML half is: risk/cost.hpp is included by strategies and
# by ALTAIR_HOT code and must not drag tomlplusplus in. Built only when vcpkg
# has provided the parser -- the default preset has no toml++, and the cost
# calculator itself is unaffected either way.
find_package(tomlplusplus CONFIG QUIET)
if(tomlplusplus_FOUND)
    add_library(altair_charges_toml STATIC charges_toml.cpp)
    add_library(altair::charges_toml ALIAS altair_charges_toml)
    target_link_libraries(altair_charges_toml
        PUBLIC altair_risk
        PRIVATE tomlplusplus::tomlplusplus altair_flags)
    message(STATUS "altair: P3-09b charges.toml loader enabled")
else()
    message(STATUS "altair: P3-09b charges.toml loader SKIPPED "
                   "(configure with the `vcpkg` preset to enable it)")
endif()

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_cost_test tests/test_cost.cpp)
    target_link_libraries(altair_cost_test PRIVATE altair_risk altair_flags)
    add_test(NAME cost COMMAND altair_cost_test)

    add_executable(altair_slippage_test tests/test_slippage.cpp)
    target_link_libraries(altair_slippage_test PRIVATE altair_risk altair_flags)
    add_test(NAME slippage COMMAND altair_slippage_test)

    add_executable(altair_sizing_test tests/test_sizing.cpp)
    target_link_libraries(altair_sizing_test PRIVATE altair_risk altair_flags)
    add_test(NAME sizing COMMAND altair_sizing_test)

    add_executable(altair_limits_test tests/test_limits.cpp)
    target_link_libraries(altair_limits_test PRIVATE altair_risk altair_flags)
    add_test(NAME limits COMMAND altair_limits_test)

    add_executable(altair_hedge_book_test tests/test_hedge_book.cpp)
    target_link_libraries(altair_hedge_book_test PRIVATE altair_risk altair_strategies altair_flags)
    add_test(NAME hedge_book COMMAND altair_hedge_book_test)

    add_executable(altair_portfolio_test tests/test_portfolio.cpp)
    target_link_libraries(altair_portfolio_test PRIVATE altair_risk altair_flags)
    add_test(NAME portfolio COMMAND altair_portfolio_test)

    if(tomlplusplus_FOUND)
        # Reads the REAL config/charges.toml, so the path is a build-time
        # constant rather than something the test hunts for relative to a
        # working directory that ctest is free to change.
        add_executable(altair_charges_toml_test tests/test_charges_toml.cpp)
        target_link_libraries(altair_charges_toml_test
            PRIVATE altair_charges_toml altair_flags)
        target_compile_definitions(altair_charges_toml_test PRIVATE
            ALTAIR_CHARGES_TOML="${CMAKE_SOURCE_DIR}/config/charges.toml")
        add_test(NAME charges_toml COMMAND altair_charges_toml_test)
    endif()

    # P15-01/02. VaR three ways and Expected Shortfall. The card is the
    # DISAGREEMENT between the three, plus the subadditivity counterexample
    # that explains why Basel moved the trading book to ES.
    add_executable(altair_var_test tests/test_var.cpp)
    target_link_libraries(altair_var_test PRIVATE altair_risk altair_flags)
    target_compile_definitions(altair_var_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME risk_var COMMAND altair_var_test)

    # P15-03/05/06. Covariance estimation and portfolio optimisers, every one
    # of them shipped against the 1/N control QUANTLAB found beats them.
    add_executable(altair_optimise_test tests/test_optimise.cpp)
    target_link_libraries(altair_optimise_test PRIVATE altair_risk altair_flags)
    add_test(NAME risk_optimise COMMAND altair_optimise_test)
endif()
