set(RUNTIME ${COMPONENT}.Runtime)

##############################################################################
# Work: the intrusive node, the same-shard run queue and the cross-thread inbox
##############################################################################
add_library(${RUNTIME}.Work INTERFACE
        work/work.hpp
        work/run_queue.hpp
        work/inbox.hpp
)
target_include_directories(${RUNTIME}.Work INTERFACE
        work/
)
##############################################################################

##############################################################################
# Timers: the intrusive node and the hierarchical wheel
##############################################################################
add_library(${RUNTIME}.Timer STATIC
        timer/timer_wheel.hpp
        timer/timer_wheel.cpp
)
target_include_directories(${RUNTIME}.Timer PUBLIC
        timer/
)
##############################################################################

##############################################################################
# Runtime exported library
##############################################################################
add_combined_library(${RUNTIME}
        DIRECTORIES
        export
        SOURCES
        export/aloe/runtime
        LIBRARIES
        ${RUNTIME}.Work
        ${RUNTIME}.Timer
)

add_library(Aloe::Component::Runtime ALIAS ${RUNTIME})
##############################################################################
