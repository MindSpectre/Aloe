set(LOOP ${COMMON}.Loop)

##############################################################################
# Loop: the module aloe.loop, one partition per former header, in the folder
# the header was in.
#   work: the intrusive node, the same-thread run queue and the cross-thread
#     inbox.
#   timer: the intrusive timer node and the hierarchical wheel (the wheel's
#     members are defined in timer_wheel.cpp).
#   counters: the shard's counters.
#   queue: one device queue with a bounded transmit ring.
# The umbrella export/aloe/loop imports it.
##############################################################################
add_library(${LOOP} STATIC
        timer/timer_wheel.cpp
        export/aloe/loop
)
target_sources(${LOOP} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        loop.cppm
        work/work.cppm
        work/run_queue.cppm
        work/inbox.cppm
        timer/timer_wheel.cppm
        counters/counters.cppm
        queue/shard_queue.cppm
)
target_include_directories(${LOOP} PUBLIC
        export
)
target_link_libraries(${LOOP} PUBLIC
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Wire
)

add_library(Aloe::Common::Loop ALIAS ${LOOP})
##############################################################################
