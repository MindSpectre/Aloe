set(RUNTIME ${COMMON}.Runtime)

##############################################################################
# The shard's counters: the queue's, and what the runtime counts beside them
##############################################################################
add_library(${RUNTIME}.Counters INTERFACE
        counters/shard_counters.hpp
)
target_include_directories(${RUNTIME}.Counters INTERFACE
        counters/
)
target_link_libraries(${RUNTIME}.Counters INTERFACE
        Aloe::Common::Loop
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
        Aloe::Common::Execution
        Aloe::Common::Log
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
        ${RUNTIME}.Counters
        Aloe::Common::Loop
        Aloe::Common::Core
        Aloe::Common::Log
        Aloe::Common::Wire
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
        Aloe::Common::Execution
        Aloe::Common::Loop
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
        Aloe::Common::Execution
)
##############################################################################

##############################################################################
# The stream owners, wait slots and senders over the TCP brick
##############################################################################
add_library(${RUNTIME}.Stream INTERFACE
        stream/stream_wait.hpp
        stream/stream_policies.hpp
        stream/streams.hpp
        stream/stream_senders.hpp
        stream/stream_handle.hpp
)
target_include_directories(${RUNTIME}.Stream INTERFACE
        stream/
)
target_link_libraries(${RUNTIME}.Stream INTERFACE
        ${RUNTIME}.Scheduler
        Aloe::Common::Tcp
        Aloe::Common::Stream
        Aloe::Common::Net
        Aloe::Common::Core
        Aloe::Common::Execution
        Aloe::Common::Loop
)
##############################################################################

##############################################################################
# The ready-made TCP stack and the ARP forwarding between shards
##############################################################################
add_library(${RUNTIME}.Stack INTERFACE
        stack/arp_forward.hpp
        stack/tcp_runtime_stack.hpp
)
target_include_directories(${RUNTIME}.Stack INTERFACE
        stack/
)
target_link_libraries(${RUNTIME}.Stack INTERFACE
        ${RUNTIME}.Stream
        ${RUNTIME}.Context
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Loop
        Aloe::Common::Wire
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
        ${RUNTIME}.Counters
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Execution
        Aloe::Common::Log
        Aloe::Common::Loop
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
        ${RUNTIME}.Counters
        ${RUNTIME}.Scope
        ${RUNTIME}.Context
        ${RUNTIME}.Scheduler
        ${RUNTIME}.Task
        ${RUNTIME}.Stream
        ${RUNTIME}.Stack
        ${RUNTIME}.Shard
        ${RUNTIME}.Runtime
        Aloe::Common::Loop
)

add_library(Aloe::Common::Runtime ALIAS ${RUNTIME})
##############################################################################
