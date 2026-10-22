add_subdirectory(bootstrap)

add_subdirectory(dpdk)

add_subdirectory(hooks)

if (DO_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
