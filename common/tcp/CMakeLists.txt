set(TCP ${COMMON}.Tcp)

##############################################################################
# The connection table: a fixed open-addressing index, backend-independent
##############################################################################
add_library(${TCP}.Table STATIC
        table/flow_table.hpp
        table/flow_table.cpp
)
target_include_directories(${TCP}.Table PUBLIC
        table/
)
target_link_libraries(${TCP}.Table PUBLIC
        Aloe::Common::Wire
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# The brick: its configuration and counters, the congestion policies, the shard's node pools, one connection's
# parts and the stack that drives them
##############################################################################
add_library(${TCP}.Stack INTERFACE
        config/tcp_config.hpp
        config/tcp_counters.hpp
        congestion/congestion_control.hpp
        congestion/newreno.hpp
        congestion/no_congestion_control.hpp
        pool/node_pool.hpp
        pool/held_segment_pool.hpp
        pool/sent_segment_pool.hpp
        connection/ack_policy.hpp
        connection/frame_template.hpp
        connection/loss_recovery.hpp
        connection/read_view.hpp
        connection/receive_queue.hpp
        connection/retransmission_queue.hpp
        connection/rtt_estimator.hpp
        connection/send_sequence.hpp
        connection/tcp_timestamps.hpp
        connection/zero_window_probe.hpp
        connection/tcp_connection.hpp
        stack/connection_table.hpp
        stack/isn_generator.hpp
        stack/segment_writer.hpp
        stack/tcp_fwd.hpp
        stack/tcp_stack.hpp
)
target_include_directories(${TCP}.Stack INTERFACE
        config/
        congestion/
        pool/
        connection/
        stack/
)
target_link_libraries(${TCP}.Stack INTERFACE
        ${TCP}.Table
        Aloe::Common::Net
        Aloe::Common::Stream
        Aloe::Common::Loop
        Aloe::Common::Device
        Aloe::Common::Wire
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# Tcp exported library
##############################################################################
add_combined_library(${TCP}
        DIRECTORIES
        export
        SOURCES
        export/aloe/tcp
        LIBRARIES
        ${TCP}.Table
        ${TCP}.Stack
)

add_library(Aloe::Common::Tcp ALIAS ${TCP})
##############################################################################
