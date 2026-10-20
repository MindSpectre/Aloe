##############################################################################
# The benchmark harness's pure pieces: percentiles, the application work, the stamp wrapper over a fabric port,
# the options, hugepage reservation over a fake sysfs, and the result file
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Benchmarks
        test_distribution.cpp
        test_application_work.cpp
        test_stamped_device.cpp
        test_benchmark_options.cpp
        test_stack_config.cpp
        test_hugepages.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Benchmarks
        PRIVATE
        Aloe::Benchmarks::Harness
        Aloe::Fixtures::Fabric
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
