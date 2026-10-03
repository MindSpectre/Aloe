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
# Counters
##############################################################################
add_library(${RUNTIME}.Counters INTERFACE
        counters/counters.hpp
)
target_include_directories(${RUNTIME}.Counters INTERFACE
        counters/
)
##############################################################################

##############################################################################
# The per-shard counting scope
##############################################################################
add_library(${RUNTIME}.Scope STATIC
        scope/task_scope.hpp
        scope/task_scope.cpp
)
target_include_directories(${RUNTIME}.Scope PUBLIC
        scope/
)
target_link_libraries(${RUNTIME}.Scope PUBLIC
        ${RUNTIME}.Counters
        Aloe::Common::Core
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
        ${RUNTIME}.Counters
        ${RUNTIME}.Scope
)

add_library(Aloe::Component::Runtime ALIAS ${RUNTIME})
##############################################################################
