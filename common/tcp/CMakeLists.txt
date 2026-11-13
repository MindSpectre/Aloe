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
        connection/detail/ack_policy.hpp
        connection/detail/connection_list.hpp
        connection/detail/control_block.hpp
        connection/detail/frame_template.hpp
        connection/detail/loss_recovery.hpp
        connection/detail/receive_queue.hpp
        connection/detail/receive_window.hpp
        connection/detail/retransmission_queue.hpp
        connection/detail/rtt_estimator.hpp
        connection/detail/sack_blocks.hpp
        connection/detail/send_sequence.hpp
        connection/detail/tcp_timestamps.hpp
        connection/detail/zero_window_probe.hpp
        connection/read_view.hpp
        connection/tcp_connection.hpp
        pool/detail/held_segment_pool.hpp
        pool/detail/node_pool.hpp
        pool/detail/sent_segment_pool.hpp
        stack/detail/ack_processor.hpp
        stack/detail/connection_table.hpp
        stack/detail/isn_generator.hpp
        stack/detail/lifecycle.hpp
        stack/detail/receiver.hpp
        stack/detail/segment_writer.hpp
        stack/detail/stack_context.hpp
        stack/detail/transmitter.hpp
        stack/tcp_fwd.hpp
        stack/tcp_stack.hpp
)
target_include_directories(${TCP}.Stack INTERFACE
        config/
        congestion/
        connection/
        pool/
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
