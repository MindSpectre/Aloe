##############################################################################
# Test Frames: the TCP builder, the parser field and the scripted peer
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Frames
        test_tcp_frames.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Frames
        PRIVATE
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
