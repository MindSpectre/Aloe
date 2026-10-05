##############################################################################
# Test Wire: addresses, byte order, checksums and the header formats
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Wire
        test_mac_address.cpp
        test_ipv4_address.cpp
        test_bytes.cpp
        test_checksum.cpp
        test_wire_formats.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Wire
        PRIVATE
        Aloe::Common::Wire
        ${TEST_LIBS}
)
##############################################################################
