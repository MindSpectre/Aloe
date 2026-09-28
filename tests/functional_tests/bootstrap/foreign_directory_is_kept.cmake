# Usage: cmake -DSHIM=<cmake/vcpkg-bootstrap.cmake> -DWORK_DIR=<scratch dir> -P foreign_directory_is_kept.cmake
#
# A `vcpkg` directory in the source tree that is not a vcpkg checkout belongs to
# someone else: a clone still running in another terminal, a half-restored
# copy, files a developer parked there. Configure must stop, name the
# directory, and leave it and its contents exactly as they were.

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/repo/cmake")
file(COPY "${SHIM}" DESTINATION "${WORK_DIR}/repo/cmake")
file(WRITE "${WORK_DIR}/repo/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.29)\n"
        "project(probe LANGUAGES NONE)\n")

set(foreign_file "${WORK_DIR}/repo/vcpkg/notes.txt")
file(WRITE "${foreign_file}" "not a vcpkg checkout\n")

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=VCPKG_ROOT
        "${CMAKE_COMMAND}" -G Ninja -S "${WORK_DIR}/repo" -B "${WORK_DIR}/build"
        "-DCMAKE_TOOLCHAIN_FILE=${WORK_DIR}/repo/cmake/vcpkg-bootstrap.cmake"
        "-DVCPKG_BOOTSTRAP_URL=${WORK_DIR}/no-such-repository"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
)
string(REGEX REPLACE "[ \t\r\n]+" " " flat_output "${output}")

if (NOT EXISTS "${foreign_file}")
    message(FATAL_ERROR "the existing vcpkg directory was deleted:\n${output}")
endif ()
if (result EQUAL 0)
    message(FATAL_ERROR "configure succeeded with a vcpkg directory that is not a checkout:\n${output}")
endif ()
if (NOT flat_output MATCHES "exists but is not a vcpkg checkout")
    message(FATAL_ERROR "the failure does not say the directory is not a vcpkg checkout:\n${output}")
endif ()
