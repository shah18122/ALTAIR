# research/ - the paper pipeline: ingest, feature card, scaffold, replication,
# promotion gate.
#
# papers/inbox/ is where PDFs get dropped. Nothing here reads a PDF: turning a
# paper into a feature card is a reading task, and the card is what the code
# operates on.
#
# Links no oms/ header. This directory decides whether an idea is worth
# trading; the trade handler decides how.

add_library(altair_research INTERFACE)
add_library(altair::research ALIAS altair_research)

target_include_directories(altair_research INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_research INTERFACE
    altair_types
    altair_time
    altair_analytics
    altair_features
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_research_test tests/test_research.cpp)
    target_link_libraries(altair_research_test PRIVATE altair_research altair_flags)
    add_test(NAME research_pipeline COMMAND altair_research_test)

    # Paper replications that showed no tradable edge (docs/research/
    # research-ideas.md). Kept, tested, and out of the production folders.
    add_executable(altair_action_principle_test tests/test_action_principle.cpp)
    target_link_libraries(altair_action_principle_test PRIVATE altair_research altair_flags)
    add_test(NAME action_principle COMMAND altair_action_principle_test
             ${CMAKE_SOURCE_DIR}/dataset/spot/nifty/1d/all.csv)

    add_executable(altair_quantum_oscillator_test tests/test_quantum_oscillator.cpp)
    target_link_libraries(altair_quantum_oscillator_test PRIVATE altair_research altair_flags)
    add_test(NAME quantum_oscillator COMMAND altair_quantum_oscillator_test
             ${CMAKE_SOURCE_DIR}/dataset/spot/nifty/1d/all.csv)

    add_executable(altair_quantum_walk_test tests/test_quantum_walk.cpp)
    target_link_libraries(altair_quantum_walk_test PRIVATE altair_research altair_flags)
    add_test(NAME quantum_walk COMMAND altair_quantum_walk_test)

    add_executable(altair_gamma_scalping_test tests/test_gamma_scalping.cpp)
    target_link_libraries(altair_gamma_scalping_test PRIVATE altair_research altair_flags)
    add_test(NAME gamma_scalping COMMAND altair_gamma_scalping_test)

    add_executable(altair_parabolic_sar_test tests/test_parabolic_sar.cpp)
    target_link_libraries(altair_parabolic_sar_test PRIVATE altair_research altair_flags)
    add_test(NAME parabolic_sar COMMAND altair_parabolic_sar_test
             ${CMAKE_SOURCE_DIR}/dataset/spot/nifty/1d/all.csv)

    add_executable(altair_stochastic_dominance_test tests/test_stochastic_dominance.cpp)
    target_link_libraries(altair_stochastic_dominance_test PRIVATE altair_research altair_flags)
    add_test(NAME stochastic_dominance COMMAND altair_stochastic_dominance_test
             ${CMAKE_SOURCE_DIR}/dataset/spot/nifty/1d/all.csv
             ${CMAKE_SOURCE_DIR}/dataset/spot/banknifty/1d/all.csv)

    add_executable(altair_anfis_test tests/test_anfis.cpp)
    target_link_libraries(altair_anfis_test PRIVATE altair_research altair_flags)
    add_test(NAME models_anfis COMMAND altair_anfis_test
             ${CMAKE_SOURCE_DIR}/dataset/spot/nifty/1d/all.csv)

    # Forsyth (2011), optimal execution by an HJB equation: checked against
    # the closed-form TWAP, the paper's Table 7.3, and every Almgren-Chriss
    # trajectory on the same grid.
    add_executable(altair_hjb_execution_test tests/test_hjb_execution.cpp)
    target_link_libraries(altair_hjb_execution_test PRIVATE altair_research altair_flags)
    add_test(NAME oms_hjb_execution COMMAND altair_hjb_execution_test)

    # Lai (2026), Almgren-Chriss with an Ornstein-Uhlenbeck alpha signal:
    # the closed form against its own ODE, then against plain AC on paths.
    add_executable(altair_alpha_execution_test tests/test_alpha_execution.cpp)
    target_link_libraries(altair_alpha_execution_test PRIVATE altair_research altair_flags)
    add_test(NAME oms_alpha_execution COMMAND altair_alpha_execution_test)

    # Verma (VRC), the execution state model and its opportunistic slicer,
    # against TWAP under the paper's own regime-conditioned cost.
    add_executable(altair_execution_state_test tests/test_execution_state.cpp)
    target_link_libraries(altair_execution_state_test PRIVATE altair_research altair_flags)
    add_test(NAME oms_execution_state COMMAND altair_execution_state_test)
endif()
