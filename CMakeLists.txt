cmake_minimum_required(VERSION 3.30.5)

project("Aloe" LANGUAGES CXX VERSION 0.1.0)

include(cmake/utils.cmake)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The modules experiment (branch experiment/modules) converts Aloe's modules to
# C++20 named modules, so CMake scans every file for module dependencies.
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "This branch builds C++ modules with clang only: GCC 16.1.1 crashes in modules mode on any "
            "file that contains stdexec (GCC PR 128318, ledger draft 01 in docs/superpowers/experiments/modules/issues/).")
endif ()

# clang-tidy replays the compiler's command line through the clang front end, so
# it only runs when the compiler is clang. Under GCC it would reject GCC-only flags.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_CLANG_TIDY
            clang-tidy
            --header-filter=${CMAKE_SOURCE_DIR}/.*
            --warnings-as-errors=*
    )
endif ()

# ── Tool versions ────────────────────────────────────────────────────────────
query_tool_version(CLANG_VERSION clang++ --version)
query_tool_version(GCC_VERSION g++ --version)
query_tool_version(CMAKE_VER_STR cmake --version)
query_tool_version(NINJA_VERSION ninja --version)
query_tool_version(CCACHE_VERSION ccache --version)
query_tool_version(MOLD_VERSION mold --version)

# cmake/vcpkg-bootstrap.cmake (the toolchain file every preset points at) resolves
# the checkout and reports it here. Fall back to back-deriving the root for builds
# that aim CMAKE_TOOLCHAIN_FILE straight at vcpkg/scripts/buildsystems/vcpkg.cmake.
if (DEFINED ALOE_VCPKG_ROOT)
    set(_vcpkg_root "${ALOE_VCPKG_ROOT}")
else ()
    get_filename_component(_vcpkg_root "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
    get_filename_component(_vcpkg_root "${_vcpkg_root}/../.." ABSOLUTE)
    set(ALOE_VCPKG_ORIGIN "CMAKE_TOOLCHAIN_FILE")
endif ()
vcpkg_commit_hash(VCPKG_COMMIT "${_vcpkg_root}")

PrintLineSeparator()
message("\tTOOLCHAIN")
message("Clang:    ${CLANG_VERSION}")
message("GCC:      ${GCC_VERSION}")
message("CMake:    ${CMAKE_VER_STR}")
message("Ninja:    ${NINJA_VERSION}")
message("ccache:   ${CCACHE_VERSION}")
message("mold:     ${MOLD_VERSION}")
PrintLineSeparator()

message("\tBUILD CONFIGURATION")
message("Build type:   ${CMAKE_BUILD_TYPE}")
message("C++ standard: ${CMAKE_CXX_STANDARD} (required: ${CMAKE_CXX_STANDARD_REQUIRED})")
message("CXX flags:    ${CMAKE_CXX_FLAGS}")
message("Linker flags: ${CMAKE_EXE_LINKER_FLAGS}")
message("Compiler:     ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
PrintLineSeparator()

message("\tVCPKG")
message("Toolchain: ${CMAKE_TOOLCHAIN_FILE}")
message("Root:      ${_vcpkg_root} (via ${ALOE_VCPKG_ORIGIN})")
message("Triplet:   ${VCPKG_TARGET_TRIPLET}")
message("Commit:    ${VCPKG_COMMIT}")
PrintLineSeparator()

message("\tGIT HOOKS")
include(cmake/git_hooks.cmake)
install_git_hooks("${CMAKE_SOURCE_DIR}")
PrintLineSeparator()

set(THREADS_PREFER_PTHREAD_FLAG ON)

# Toggles. Included before the compile options because KEEP_FRAME_POINTERS
# decides one of them.
include(cmake/features.cmake)

list(APPEND general_compile_options
        -Wall -Wextra -Wpedantic
        -Wconversion -Wshadow
        -Wdouble-promotion -Wnull-dereference
        -Wnon-virtual-dtor
        -Werror
        -fno-stack-protector
        -fvisibility=hidden
        -g
)

# The thread-safety analysis exists only in clang.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(APPEND general_compile_options -Wthread-safety)
    # CMake 4.3's BMI-only command passes flags clang then calls unused, which -Werror
    # turns into an error (ledger 05).
    list(APPEND general_compile_options -Wno-unused-command-line-argument)
    # quill's package adds this to every target that links it. Set for all targets, so quill's consumers do not differ
    # from the modules they import by a flag, and CMake builds no synthesized BMIs for them (ledgers 08 and 10).
    list(APPEND general_compile_options -Wno-gnu-zero-variadic-macro-arguments)
endif ()

# Target compile options land after CMAKE_CXX_FLAGS on the command line, so a
# preset that merely adds -fno-omit-frame-pointer to the flags loses to this.
# The `release-perf` preset flips this option instead.
if (KEEP_FRAME_POINTERS)
    list(APPEND general_compile_options -fno-omit-frame-pointer)
else ()
    list(APPEND general_compile_options $<$<CONFIG:Release>:-fomit-frame-pointer>)
endif ()

add_compile_options(
        ${general_compile_options}
)

message("\tCOMPILE OPTIONS")
message("Compile options are ${general_compile_options}")
PrintLineSeparator()

#custom directives
include(cmake/tests.cmake)
include(cmake/combined_library.cmake)

message("\tPACKAGES")
find_package(PkgConfig REQUIRED)
message("Using PkgConfig")
find_package(Threads REQUIRED)
message("Using Threads")
find_package(stdexec CONFIG REQUIRED)
message("Using stdexec")
find_package(quill CONFIG REQUIRED)
message("Using quill")
include(cmake/dpdk.cmake)
message("Using DPDK ${ALOE_DPDK_VERSION}")
PrintLineSeparator()

message("\tPROJECT TARGETS LOGS")

add_subdirectory(common)
add_subdirectory(fixtures)

##############################################################################
# TESTS
##############################################################################
if (USE_TESTS)
    message("Tests are enabled")
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif ()
##############################################################################
##############################################################################
# Benchmarks
##############################################################################
if (DO_BENCHMARKS)
    message("Benchmarks are enabled")
    find_package(benchmark CONFIG REQUIRED)
    add_subdirectory(benchmarks)
endif ()
##############################################################################

##############################################################################
# Examples
##############################################################################
if (BUILD_EXAMPLES)
    message("Examples are enabled")
    add_subdirectory(examples)
endif ()
##############################################################################
##############################################################################
# Documentation
##############################################################################
if (BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    message("Documentation target enabled (doxygen ${DOXYGEN_VERSION})")
    add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/build/docs
            COMMAND Doxygen::doxygen ${CMAKE_SOURCE_DIR}/docs/doxygen/Doxyfile
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
    )
endif ()
##############################################################################

PrintLineSeparator()
