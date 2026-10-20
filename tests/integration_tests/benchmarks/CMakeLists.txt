##############################################################################
# The benchmark harness over DPDK's ring driver: the ring pair, the scripted venue and the whole run, no root
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Benchmarks.RingPair
        test_ring_pair.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Benchmarks.RingPair
        PRIVATE
        Aloe::Benchmarks::Harness
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
