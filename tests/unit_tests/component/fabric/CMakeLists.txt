##############################################################################
# Test Fabric: packets, delivery, steering and emulated offloads
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Fabric
        test_fabric_conformance.cpp
        test_fabric_delivery.cpp
        test_fabric_offloads.cpp
        test_fabric_packet.cpp
        test_fabric_steering.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Fabric
        PRIVATE
        Aloe::Component::Fabric
        ${SHARED_TESTING_TARGET}.Device
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Fabric: delivery from many threads at once, for the tsan preset
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Fabric.Threads
        test_fabric_threads.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Fabric.Threads
        PRIVATE
        Aloe::Component::Fabric
        ${SHARED_TESTING_TARGET}.Device
        ${TEST_LIBS}
)
##############################################################################
