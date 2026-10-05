##############################################################################
# Test Net: the wire formats, the ARP cache and the brick over a fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Net
        test_net_wire.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Net
        PRIVATE
        Aloe::Common::Net
        ${TEST_LIBS}
)
##############################################################################
