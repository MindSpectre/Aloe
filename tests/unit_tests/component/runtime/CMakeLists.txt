##############################################################################
# Test Runtime: the context's primitives, the scheduler, the shard and the runtime on the fabric
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Runtime
        test_inbox.cpp
        test_run_queue.cpp
        test_shard_context.cpp
        test_task_scope.cpp
        test_timer_wheel.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Runtime
        PRIVATE
        Aloe::Component::Runtime
        ${SHARED_TESTING_TARGET}.Log
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Runtime: cross-thread paths, for the tsan preset
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Runtime.Threads
        test_inbox_threads.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Runtime.Threads
        PRIVATE
        Aloe::Component::Runtime
        Threads::Threads
        ${TEST_LIBS}
)
##############################################################################
