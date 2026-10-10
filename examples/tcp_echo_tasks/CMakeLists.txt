##############################################################################
# TCP echo as tasks over the runtime on every queue of a DPDK port: the runtime's TCP living example
##############################################################################
add_executable(${EXAMPLES}.TcpEchoTasks
        tcp_echo_tasks.cpp
)
target_link_libraries(${EXAMPLES}.TcpEchoTasks
        PRIVATE
        Aloe::Common::Runtime
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Ethdev
)
##############################################################################
