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
endif()
