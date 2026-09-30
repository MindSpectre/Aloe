##############################################################################
# Test Fabric: packets, delivery, steering and emulated offloads
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Fabric
        test_fabric_packet.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Fabric
        PRIVATE
        Aloe::Component::Fabric
        ${TEST_LIBS}
)
##############################################################################
