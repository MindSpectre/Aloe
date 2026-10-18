option(USE_TESTS "Tests activated" ON)
option(DO_BENCHMARKS "Benchmarks activated" ON)
option(BUILD_EXAMPLES "Build example apps" ON)
option(KEEP_FRAME_POINTERS "Keep frame pointers in Release (perf call graphs)" OFF)
option(USE_TSC_CLOCK "core::Clock reads the TSC (x86-64, invariant TSC, kernel clocksource tsc)" ON)
option(BUILD_DOCS "Generate API documentation with Doxygen (target: docs)" OFF)

# Build-wide flags the code reads, set for every target so that every
# translation unit sees the same choice.
if (USE_TSC_CLOCK)
    if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "USE_TSC_CLOCK reads the x86-64 time-stamp counter, and this target is "
                "${CMAKE_SYSTEM_PROCESSOR}; configure with -DUSE_TSC_CLOCK=OFF")
    endif ()
    add_compile_definitions(ALOE_USE_TSC_CLOCK)
endif ()
