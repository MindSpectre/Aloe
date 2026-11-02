##############################################################################
# Test Stream: the zero-copy stream contract over a stub
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Stream
        test_stream.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Stream
        PRIVATE
        Aloe::Common::Stream
        ${TEST_LIBS}
)
##############################################################################
