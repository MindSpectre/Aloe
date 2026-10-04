set(RUNTIME ${COMMON}.Runtime)

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
        Aloe::Common::Loop
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
        ${RUNTIME}.Scope
        Aloe::Common::Loop
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
# The shard: one device queue, the stack, and the tick
##############################################################################
add_library(${RUNTIME}.Shard INTERFACE
        shard/shard.hpp
)
target_include_directories(${RUNTIME}.Shard INTERFACE
        shard/
)
target_link_libraries(${RUNTIME}.Shard INTERFACE
        ${RUNTIME}.Scheduler
        Aloe::Common::Loop
        Aloe::Common::Device
        Threads::Threads
)
##############################################################################

##############################################################################
# The runtime: N shards on N threads
##############################################################################
add_library(${RUNTIME}.Runtime INTERFACE
        runtime/runtime.hpp
)
target_include_directories(${RUNTIME}.Runtime INTERFACE
        runtime/
)
target_link_libraries(${RUNTIME}.Runtime INTERFACE
        ${RUNTIME}.Shard
        Aloe::Common::Utils
        Threads::Threads
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
        ${RUNTIME}.Scope
        ${RUNTIME}.Context
        ${RUNTIME}.Scheduler
        ${RUNTIME}.Task
        ${RUNTIME}.Shard
        ${RUNTIME}.Runtime
        Aloe::Common::Loop
)

add_library(Aloe::Common::Runtime ALIAS ${RUNTIME})
##############################################################################
