set(LOOP ${COMPONENT}.Loop)

##############################################################################
# Work: the intrusive node, the same-thread run queue and the cross-thread inbox
##############################################################################
add_library(${LOOP}.Work INTERFACE
        work/work.hpp
        work/run_queue.hpp
        work/inbox.hpp
)
target_include_directories(${LOOP}.Work INTERFACE
        work/
)
##############################################################################

##############################################################################
# Timers: the intrusive node and the hierarchical wheel
##############################################################################
add_library(${LOOP}.Timer STATIC
        timer/timer_wheel.hpp
        timer/timer_wheel.cpp
)
target_include_directories(${LOOP}.Timer PUBLIC
        timer/
)
##############################################################################

##############################################################################
# Counters
##############################################################################
add_library(${LOOP}.Counters INTERFACE
        counters/counters.hpp
)
target_include_directories(${LOOP}.Counters INTERFACE
        counters/
)
##############################################################################

##############################################################################
# The queue: one device queue with a bounded transmit ring
##############################################################################
add_library(${LOOP}.Queue INTERFACE
        queue/shard_queue.hpp
)
target_include_directories(${LOOP}.Queue INTERFACE
        queue/
)
target_link_libraries(${LOOP}.Queue INTERFACE
        ${LOOP}.Counters
        Aloe::Component::Device
        Aloe::Common::Utils
)
##############################################################################

##############################################################################
# Loop exported library
##############################################################################
add_combined_library(${LOOP}
        DIRECTORIES
        export
        SOURCES
        export/aloe/loop
        LIBRARIES
        ${LOOP}.Work
        ${LOOP}.Timer
        ${LOOP}.Counters
        ${LOOP}.Queue
)

add_library(Aloe::Component::Loop ALIAS ${LOOP})
##############################################################################
