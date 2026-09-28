# vcpkg bootstrap shim.
#
# Every preset points CMAKE_TOOLCHAIN_FILE at this file rather than directly at
# vcpkg/scripts/buildsystems/vcpkg.cmake, so a fresh clone configures without the
# user provisioning vcpkg by hand. This file resolves a vcpkg checkout, clones and
# bootstraps one if there is none, then chains to the real vcpkg toolchain.
#
# Resolution order:
#   1. -DVCPKG_ROOT=<path>   - an existing checkout explicitly selected
#   2. <repo>/vcpkg          - an existing checkout in the source tree
#   3. $ENV{VCPKG_ROOT}      - shared/system checkout (the toolchain image sets this)
#   4. otherwise             - clone + bootstrap into <repo>/vcpkg
#
# Knobs:
#   VCPKG_BOOTSTRAP_URL - clone source (default: upstream microsoft/vcpkg)
#   VCPKG_BOOTSTRAP_REF - commit/tag/branch to check out. Empty (the default)
#                         tracks upstream's default branch via a shallow clone,
#                         matching infrastructure/toolchain/Dockerfile. Setting it
#                         forces a full clone, since an arbitrary sha needs history.
#
# CMake re-reads the toolchain file for every try_compile, so everything below must
# stay cheap and idempotent. Explicit roots are forwarded to compiler checks so
# they reuse the same checkout instead of provisioning one in the source tree.

# Derived from this file's own location rather than CMAKE_SOURCE_DIR, which is not
# the repository root inside try_compile sub-projects.
get_filename_component(ALOE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(VCPKG_BOOTSTRAP_URL "https://github.com/microsoft/vcpkg" CACHE STRING
        "Repository the vcpkg bootstrap clones from")
set(VCPKG_BOOTSTRAP_REF "" CACHE STRING
        "vcpkg commit/tag/branch to check out; empty tracks the default branch")

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VCPKG_ROOT)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

# Relative overrides must have the same meaning inside try_compile sub-projects.
if (DEFINED VCPKG_ROOT AND NOT VCPKG_ROOT STREQUAL "")
    get_filename_component(VCPKG_ROOT "${VCPKG_ROOT}" ABSOLUTE BASE_DIR "${ALOE_SOURCE_ROOT}")
endif ()

set(_av_root "")
set(_av_origin "")

if (DEFINED VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(_av_root "${VCPKG_ROOT}")
    set(_av_origin "-DVCPKG_ROOT")
elseif (EXISTS "${ALOE_SOURCE_ROOT}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(_av_root "${ALOE_SOURCE_ROOT}/vcpkg")
    set(_av_origin "source tree")
elseif (DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(_av_root "$ENV{VCPKG_ROOT}")
    set(_av_origin "VCPKG_ROOT environment variable")
endif ()

# vcpkg caches its own root; do not silently mix it with a changed override.
if (_av_root AND DEFINED Z_VCPKG_ROOT_DIR)
    get_filename_component(_av_cached_root "${Z_VCPKG_ROOT_DIR}" REALPATH)
    get_filename_component(_av_selected_root "${_av_root}" REALPATH)
    if (NOT _av_cached_root STREQUAL _av_selected_root)
        message(FATAL_ERROR "vcpkg root changed. Reconfigure with --fresh or use a new build directory.")
    endif ()
endif ()

# ── Provision ────────────────────────────────────────────────────────────────

if (NOT _av_root)
    set(_av_dest "${ALOE_SOURCE_ROOT}/vcpkg")

    # Something is there but it is not a checkout: a clone still running in
    # another terminal, a half-restored copy, files someone parked there. It is
    # not ours to replace, and the failure paths below would delete it.
    if (EXISTS "${_av_dest}")
        message(FATAL_ERROR
                "vcpkg: ${_av_dest} exists but is not a vcpkg checkout "
                "(it has no scripts/buildsystems/vcpkg.cmake).\n"
                "Move it aside so a fresh checkout can be provisioned, or select an existing "
                "checkout with -DVCPKG_ROOT=<path>.")
    endif ()

    find_program(ALOE_GIT_EXECUTABLE git)
    if (NOT ALOE_GIT_EXECUTABLE)
        message(FATAL_ERROR
                "No vcpkg checkout found and git is not installed, so one cannot be provisioned.\n"
                "Install git and re-run, or provide a vcpkg checkout yourself:\n"
                "  git clone ${VCPKG_BOOTSTRAP_URL} ${_av_dest} && ${_av_dest}/bootstrap-vcpkg.sh\n"
                "An existing checkout elsewhere can be reused with -DVCPKG_ROOT=<path>.")
    endif ()

    if (VCPKG_BOOTSTRAP_REF)
        # A full clone: an arbitrary commit is not reachable from a shallow one.
        set(_av_clone_args "")
        message(STATUS "vcpkg: not found, cloning ${VCPKG_BOOTSTRAP_URL}@${VCPKG_BOOTSTRAP_REF} into ${_av_dest}")
    else ()
        set(_av_clone_args --depth 1)
        message(STATUS "vcpkg: not found, cloning ${VCPKG_BOOTSTRAP_URL} into ${_av_dest}")
    endif ()

    execute_process(
            COMMAND "${ALOE_GIT_EXECUTABLE}" clone ${_av_clone_args} "${VCPKG_BOOTSTRAP_URL}" "${_av_dest}"
            RESULT_VARIABLE _av_rc
    )
    if (NOT _av_rc EQUAL 0)
        # Leave no half-clone behind: it would satisfy the step 1 check on the next
        # configure and fail far less legibly than this does.
        file(REMOVE_RECURSE "${_av_dest}")
        message(FATAL_ERROR "vcpkg: clone of ${VCPKG_BOOTSTRAP_URL} failed (exit ${_av_rc}).")
    endif ()

    if (VCPKG_BOOTSTRAP_REF)
        execute_process(
                COMMAND "${ALOE_GIT_EXECUTABLE}" -C "${_av_dest}" checkout --quiet "${VCPKG_BOOTSTRAP_REF}"
                RESULT_VARIABLE _av_rc
        )
        if (NOT _av_rc EQUAL 0)
            file(REMOVE_RECURSE "${_av_dest}")
            message(FATAL_ERROR "vcpkg: checkout of VCPKG_BOOTSTRAP_REF '${VCPKG_BOOTSTRAP_REF}' failed (exit ${_av_rc}).")
        endif ()
    endif ()

    set(_av_root "${_av_dest}")
    set(_av_origin "bootstrap clone")
endif ()

# ── Bootstrap the vcpkg executable ───────────────────────────────────────────
# Also covers a checkout that was cloned but never bootstrapped.

if (CMAKE_HOST_WIN32)
    set(_av_exe "${_av_root}/vcpkg.exe")
    set(_av_bootstrap "${_av_root}/bootstrap-vcpkg.bat")
else ()
    set(_av_exe "${_av_root}/vcpkg")
    set(_av_bootstrap "${_av_root}/bootstrap-vcpkg.sh")
endif ()

if (NOT EXISTS "${_av_exe}")
    message(STATUS "vcpkg: bootstrapping ${_av_bootstrap} (this takes a minute)")
    execute_process(
            COMMAND "${_av_bootstrap}" -disableMetrics
            WORKING_DIRECTORY "${_av_root}"
            RESULT_VARIABLE _av_rc
    )
    if (NOT _av_rc EQUAL 0)
        message(FATAL_ERROR "vcpkg: ${_av_bootstrap} failed (exit ${_av_rc}).")
    endif ()
endif ()

# ── Chain to the real toolchain ──────────────────────────────────────────────

# Cached so the top-level CMakeLists can report which checkout the build used;
# try_compile sub-projects receive the explicit VCPKG_ROOT override as well.
set(ALOE_VCPKG_ROOT "${_av_root}" CACHE INTERNAL "Resolved vcpkg root")
set(ALOE_VCPKG_ORIGIN "${_av_origin}" CACHE INTERNAL "How the vcpkg root was resolved")

include("${_av_root}/scripts/buildsystems/vcpkg.cmake")
