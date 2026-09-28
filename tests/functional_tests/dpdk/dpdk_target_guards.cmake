# Usage: cmake -DDPDK_MODULE=<cmake/dpdk.cmake> -DWORK_DIR=<scratch dir> -P dpdk_target_guards.cmake
#
# cmake/dpdk.cmake must refuse a DPDK it cannot link safely, and say why,
# instead of producing a program that fails later at startup. Each case feeds
# it made-up pkg-config data that describes one such DPDK.

# Configures a throwaway project that includes the module and checks the failure.
#   name      directory for this case
#   libs      the Libs line of the made-up libdpdk.pc
#   cflags    the Cflags line of the made-up libdpdk.pc
#   expected  regular expression the output must match
function(expect_rejection name libs cflags expected)
    set(case_dir "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${case_dir}/prefix/lib/pkgconfig" "${case_dir}/project")
    # find_library looks at file names only, so an empty file stands in for an archive.
    file(WRITE "${case_dir}/prefix/lib/librte_made_up.a" "")
    file(WRITE "${case_dir}/prefix/lib/pkgconfig/libdpdk.pc"
            "prefix=${case_dir}/prefix\n"
            "libdir=\${prefix}/lib\n"
            "Name: DPDK\n"
            "Description: made-up DPDK for a guard test\n"
            "Version: 0.0.0\n"
            "Libs: -L\${libdir} ${libs}\n"
            "Cflags: ${cflags}\n")
    file(WRITE "${case_dir}/project/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.29)\n"
            "project(probe LANGUAGES CXX)\n"
            "find_package(Threads REQUIRED)\n"
            "include(\"${DPDK_MODULE}\")\n")

    execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${case_dir}/prefix/lib/pkgconfig"
            "${CMAKE_COMMAND}" -G Ninja -S "${case_dir}/project" -B "${case_dir}/build"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output
    )

    if (result EQUAL 0)
        message(FATAL_ERROR "${name}: the DPDK module accepted a DPDK it cannot link safely:\n${output}")
    endif ()
    if (NOT output MATCHES "${expected}")
        message(FATAL_ERROR "${name}: the output does not match '${expected}':\n${output}")
    endif ()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Tuned to the machine that built it: a cached copy would not start elsewhere.
expect_rejection(tuned_to_build_machine "-llibrte_made_up.a" "-march=native" "built with -march=native")

# A shared DPDK names no archives, so nothing could be linked whole.
expect_rejection(shared_build "-lrte_made_up" "-march=corei7" "names no static archives")
