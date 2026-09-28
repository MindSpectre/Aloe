# Usage: cmake -DSHIM=<cmake/vcpkg-bootstrap.cmake> -DWORK_DIR=<scratch dir> -P failed_provisioning_is_legible.cmake
#
# When vcpkg cannot be provisioned, configure must stop with a message that
# names what failed, and must leave no half-made checkout behind that a later
# configure would mistake for a real one.

find_program(GIT_EXECUTABLE git REQUIRED)

# Configures a throwaway project through a copy of the shim and checks the failure.
#   name            directory for this case
#   url, ref        values for VCPKG_BOOTSTRAP_URL and VCPKG_BOOTSTRAP_REF
#   ARGN            regular expressions the output must match
function(expect_legible_failure name url ref)
    set(case_dir "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${case_dir}/repo/cmake")
    file(COPY "${SHIM}" DESTINATION "${case_dir}/repo/cmake")
    file(WRITE "${case_dir}/repo/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.29)\n"
            "project(probe LANGUAGES NONE)\n")

    execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env --unset=VCPKG_ROOT
            "${CMAKE_COMMAND}" -G Ninja -S "${case_dir}/repo" -B "${case_dir}/build"
            "-DCMAKE_TOOLCHAIN_FILE=${case_dir}/repo/cmake/vcpkg-bootstrap.cmake"
            "-DVCPKG_BOOTSTRAP_URL=${url}"
            "-DVCPKG_BOOTSTRAP_REF=${ref}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output
    )

    if (result EQUAL 0)
        message(FATAL_ERROR "${name}: configure succeeded although vcpkg could not be provisioned:\n${output}")
    endif ()
    # CMake wraps long messages at a width that depends on the paths inside
    # them, so line breaks and indentation are collapsed before matching.
    string(REGEX REPLACE "[ \t\r\n]+" " " flat_output "${output}")
    foreach (expected IN LISTS ARGN)
        if (NOT flat_output MATCHES "${expected}")
            message(FATAL_ERROR "${name}: the output does not match '${expected}':\n${output}")
        endif ()
    endforeach ()
    if (EXISTS "${case_dir}/repo/vcpkg")
        message(FATAL_ERROR "${name}: a half-made vcpkg checkout was left behind")
    endif ()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# No network, or a wrong address: the clone itself fails.
expect_legible_failure(unreachable_source "${WORK_DIR}/no-such-repository" ""
        "vcpkg: clone of" "failed \\(exit [0-9]+\\)")

# The clone works but the requested revision does not exist. The clone has
# already created the directory by then, so this case checks that it is removed.
set(source_repository "${WORK_DIR}/source-repository")
file(MAKE_DIRECTORY "${source_repository}")
execute_process(COMMAND "${GIT_EXECUTABLE}" init --quiet "${source_repository}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_repository}" -c user.name=aloe -c user.email=aloe@example.invalid
        commit --quiet --allow-empty -m "initial"
        COMMAND_ERROR_IS_FATAL ANY
)
expect_legible_failure(unknown_revision "${source_repository}" "no-such-revision"
        "vcpkg: checkout of VCPKG_BOOTSTRAP_REF" "no-such-revision")
