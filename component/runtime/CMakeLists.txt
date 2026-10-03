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
# The shard context: queues, timers, scope and counters, with run_once
##############################################################################
add_library(${RUNTIME}.Context STATIC
        context/shard_context.hpp
        context/shard_context.cpp
)
target_include_directories(${RUNTIME}.Context PUBLIC
        context/
)
target_link_libraries(${RUNTIME}.Context PUBLIC
        ${RUNTIME}.Work
        ${RUNTIME}.Timer
        ${RUNTIME}.Scope
)
##############################################################################

##############################################################################
# The scheduler and its senders
##############################################################################
add_library(${RUNTIME}.Scheduler INTERFACE
        scheduler/scheduler.hpp
)
target_include_directories(${RUNTIME}.Scheduler INTERFACE
        scheduler/
)
target_link_libraries(${RUNTIME}.Scheduler INTERFACE
        ${RUNTIME}.Context
)
##############################################################################

##############################################################################
# The shard task
##############################################################################
add_library(${RUNTIME}.Task INTERFACE
        task/shard_task.hpp
)
target_include_directories(${RUNTIME}.Task INTERFACE
        task/
)
target_link_libraries(${RUNTIME}.Task INTERFACE
        ${RUNTIME}.Scheduler
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
        ${RUNTIME}.Context
        ${RUNTIME}.Scheduler
        ${RUNTIME}.Task
)

add_library(Aloe::Component::Runtime ALIAS ${RUNTIME})
##############################################################################
