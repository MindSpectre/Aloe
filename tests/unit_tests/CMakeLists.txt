add_subdirectory(common)
add_subdirectory(fixtures)
add_subdirectory(scenarios)

if (DO_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
