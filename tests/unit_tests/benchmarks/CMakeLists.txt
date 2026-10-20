##############################################################################
# The benchmark harness's pure pieces: percentiles, the application work, the stamp wrapper over a fabric port,
# the options, hugepage reservation over a fake sysfs, and the result file
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Benchmarks
        test_distribution.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Benchmarks
        PRIVATE
        Aloe::Benchmarks::Harness
        ${TEST_LIBS}
)
##############################################################################
