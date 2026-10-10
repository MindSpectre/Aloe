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
# The brick: configuration, counters, retained-packet storage and the stack over IP
##############################################################################
add_library(${TCP}.Stack INTERFACE
        state/tcp_config.hpp
        state/tcp_counters.hpp
        buffer/tcp_node_pool.hpp
        buffer/read_view.hpp
        stack/tcp_fwd.hpp
        stack/tcp_connection.hpp
        stack/tcp_stack.hpp
        stack/tcp_control.hpp
        stack/tcp_receive.hpp
        stack/tcp_transmit.hpp
)
target_include_directories(${TCP}.Stack INTERFACE
        state/
        buffer/
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
