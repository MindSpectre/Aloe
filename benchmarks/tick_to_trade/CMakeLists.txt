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
