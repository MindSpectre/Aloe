##############################################################################
# Test Tcp: the table, the buffers, and the brick over a fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Tcp
        test_tcp_table.cpp
        test_tcp_buffers.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Tcp
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
