##############################################################################
# Test Device: receive-side scaling and the concepts
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Device
        test_concepts.cpp
        test_rss.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Device
        PRIVATE
        Aloe::Common::Device
        ${TEST_LIBS}
)
##############################################################################
