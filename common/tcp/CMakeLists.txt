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
        connection/read_view.hpp
        connection/tcp_connection.hpp
        detail/ack_policy.hpp
        detail/connection_table.hpp
        detail/control_block.hpp
        detail/frame_template.hpp
        detail/held_segment_pool.hpp
        detail/isn_generator.hpp
        detail/loss_recovery.hpp
        detail/node_pool.hpp
        detail/receive_queue.hpp
        detail/retransmission_queue.hpp
        detail/rtt_estimator.hpp
        detail/segment_writer.hpp
        detail/send_sequence.hpp
        detail/sent_segment_pool.hpp
        detail/tcp_timestamps.hpp
        detail/zero_window_probe.hpp
        stack/tcp_fwd.hpp
        stack/tcp_stack.hpp
)
target_include_directories(${TCP}.Stack INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        config/
        congestion/
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
