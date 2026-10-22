##############################################################################
# The tick-to-trade benchmark: the harness its tests link, and the binary (Task 11)
##############################################################################
set(TICK_TO_TRADE ${BENCHMARKS}.TickToTrade)

add_library(${TICK_TO_TRADE}.Harness STATIC
        ring_pair.cpp
        benchmark_options.cpp
        venue_peer.cpp
        level_floor.cpp
        level_bricks.cpp
        level_runtime.cpp
        hugepages.cpp
        machine_info.cpp
        report.cpp
        hop.cpp
        benchmark_run.cpp
)
target_sources(${TICK_TO_TRADE}.Harness PRIVATE
        round_config.hpp
        distribution.hpp
        ring_pair.hpp
        application_work.hpp
        stamped_device.hpp
        run_state.hpp
        thread_pinning.hpp
        benchmark_options.hpp
        venue_peer.hpp
        level_floor.hpp
        stack_sanity.hpp
        level_bricks.hpp
        level_runtime.hpp
        hugepages.hpp
        machine_info.hpp
        report.hpp
        hop.hpp
        benchmark_run.hpp
)
target_include_directories(${TICK_TO_TRADE}.Harness PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(${TICK_TO_TRADE}.Harness PUBLIC
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Ethdev
        Aloe::Common::Loop
        Aloe::Common::Net
        Aloe::Common::Stream
        Aloe::Common::Tcp
        Aloe::Common::Runtime
        Aloe::Common::Wire
        Aloe::Fixtures::Frames
        Threads::Threads
)
add_library(Aloe::Benchmarks::Harness ALIAS ${TICK_TO_TRADE}.Harness)
##############################################################################
# The binary: tick_to_trade [options] [-- <EAL arguments>]
##############################################################################
execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE TICK_TO_TRADE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if (NOT TICK_TO_TRADE_REVISION)
    set(TICK_TO_TRADE_REVISION "unknown")
endif ()
add_executable(${TICK_TO_TRADE}
        main.cpp
)
target_link_libraries(${TICK_TO_TRADE}
        PRIVATE
        ${TICK_TO_TRADE}.Harness
)
target_compile_definitions(${TICK_TO_TRADE}
        PRIVATE
        ALOE_BENCHMARK_REVISION="${TICK_TO_TRADE_REVISION}"
        ALOE_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
set_target_properties(${TICK_TO_TRADE} PROPERTIES OUTPUT_NAME tick_to_trade)
##############################################################################
