##############################################################################
# The tick-to-trade benchmark: the harness its tests link, and the binary (Task 11)
##############################################################################
set(TICK_TO_TRADE ${BENCHMARKS}.TickToTrade)

add_library(${TICK_TO_TRADE}.Harness INTERFACE
        round_config.hpp
        distribution.hpp
)
target_include_directories(${TICK_TO_TRADE}.Harness INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(${TICK_TO_TRADE}.Harness INTERFACE
        Aloe::Common::Core
)
add_library(Aloe::Benchmarks::Harness ALIAS ${TICK_TO_TRADE}.Harness)
##############################################################################
