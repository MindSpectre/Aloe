##############################################################################
# Test Runtime: the context's primitives, the scheduler, the shard and the runtime on the fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Runtime
        test_run_queue.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Runtime
        PRIVATE
        Aloe::Component::Runtime
        ${TEST_LIBS}
)
##############################################################################
