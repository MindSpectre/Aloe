##############################################################################
# Test Loop: the bricks on their own, with no device and no asynchronous model
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Loop
        test_inbox.cpp
        test_run_queue.cpp
        test_timer_wheel.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Loop
        PRIVATE
        Aloe::Component::Loop
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Loop: the inbox under many producers, for the tsan preset
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Loop.Threads
        test_inbox_threads.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Loop.Threads
        PRIVATE
        Aloe::Component::Loop
        Threads::Threads
        ${TEST_LIBS}
)
##############################################################################
