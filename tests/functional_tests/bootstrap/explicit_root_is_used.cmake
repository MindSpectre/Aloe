# Usage: cmake -DSHIM=<cmake/vcpkg-bootstrap.cmake> -DWORK_DIR=<scratch dir> -P explicit_root_is_used.cmake
#
# A vcpkg checkout named with -DVCPKG_ROOT must be used as it is: no clone, and
# the project must learn which checkout was chosen and why.

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/repo/cmake")
file(COPY "${SHIM}" DESTINATION "${WORK_DIR}/repo/cmake")
file(WRITE "${WORK_DIR}/repo/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.29)\n"
        "project(probe LANGUAGES NONE)\n"
        "message(STATUS \"root=[\${ALOE_VCPKG_ROOT}] origin=[\${ALOE_VCPKG_ORIGIN}]\")\n")

# A stand-in checkout: the real toolchain file is replaced by one that only
# announces itself, and an existing `vcpkg` file means "already bootstrapped".
set(fake_root "${WORK_DIR}/fake-vcpkg")
file(MAKE_DIRECTORY "${fake_root}/scripts/buildsystems")
file(WRITE "${fake_root}/scripts/buildsystems/vcpkg.cmake" "message(STATUS \"stand-in vcpkg toolchain loaded\")\n")
file(WRITE "${fake_root}/vcpkg" "")

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=VCPKG_ROOT
        "${CMAKE_COMMAND}" -G Ninja -S "${WORK_DIR}/repo" -B "${WORK_DIR}/build"
        "-DCMAKE_TOOLCHAIN_FILE=${WORK_DIR}/repo/cmake/vcpkg-bootstrap.cmake"
        "-DVCPKG_ROOT=${fake_root}"
        "-DVCPKG_BOOTSTRAP_URL=${WORK_DIR}/no-such-repository"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
)

if (NOT result EQUAL 0)
    message(FATAL_ERROR "configure failed although a vcpkg root was given:\n${output}")
endif ()
if (NOT output MATCHES "stand-in vcpkg toolchain loaded")
    message(FATAL_ERROR "the toolchain of the given vcpkg root was not loaded:\n${output}")
endif ()
string(FIND "${output}" "root=[${fake_root}] origin=[-DVCPKG_ROOT]" reported)
if (reported EQUAL -1)
    message(FATAL_ERROR "the project was not told which vcpkg root was chosen:\n${output}")
endif ()
if (EXISTS "${WORK_DIR}/repo/vcpkg")
    message(FATAL_ERROR "vcpkg was cloned although a root was given")
endif ()
