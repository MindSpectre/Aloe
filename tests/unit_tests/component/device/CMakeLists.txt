##############################################################################
# Test Device: addresses, checksums, receive-side scaling and the concepts
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Device
        test_address.cpp
        test_bytes.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Device
        PRIVATE
        Aloe::Component::Device
        ${TEST_LIBS}
)
##############################################################################
