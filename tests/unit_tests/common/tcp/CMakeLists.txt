##############################################################################
# Test Tcp: the table, the buffers, and the brick over a fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Tcp
        test_tcp_table.cpp
        test_tcp_buffers.cpp
        test_tcp_handshake.cpp
        test_tcp_events.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Tcp
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Tcp
        ${TEST_LIBS}
)
##############################################################################
