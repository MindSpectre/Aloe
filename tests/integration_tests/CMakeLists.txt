add_subdirectory(dpdk)
add_subdirectory(ethdev)
add_subdirectory(runtime)
add_subdirectory(net)
add_subdirectory(tcp)

if (DO_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
