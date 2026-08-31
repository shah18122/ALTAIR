# book/ — the L2 order book and microstructure (P2-08, P2-09).
# Skeleton: the structure is decided, the update path is not written.
# Header-only for now; P2-09's rolling estimators may need a .cpp.

add_library(altair_book INTERFACE)
add_library(altair::book ALIAS altair_book)

target_include_directories(altair_book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_book INTERFACE
    altair_types
    altair_time
    altair_feed
    altair_instruments
    altair_flags
)
