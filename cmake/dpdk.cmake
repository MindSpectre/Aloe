# DPDK, exposed as the single target Aloe::Dpdk.
#
# DPDK ships pkg-config data only. Two things make it unsafe to consume through
# pkg_check_modules(... IMPORTED_TARGET):
#
#   1. Drivers register themselves from static constructors. Nothing refers to
#      them by name, so a normal static link drops them and the process starts
#      with no drivers at all. DPDK's link line wraps its archives in
#      --whole-archive for that reason, but an imported target stores flags and
#      libraries separately, which silently undoes the wrapping.
#   2. Its compile flags contain the pair "-include rte_config.h". As two
#      separate options the pair can be torn apart by option de-duplication.
#
# So the archives are collected here and wrapped again with the WHOLE_ARCHIVE
# link feature, and the compile flags are passed as one unit.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALOE_DPDK REQUIRED libdpdk)

set(_aloe_dpdk_archives "")
set(_aloe_dpdk_other_libraries "")
foreach (_aloe_dpdk_library IN LISTS ALOE_DPDK_STATIC_LIBRARIES)
    if (_aloe_dpdk_library MATCHES "^librte_.+\\.a$")
        # A fresh variable per archive: find_library caches its result by name.
        string(MAKE_C_IDENTIFIER "ALOE_DPDK_ARCHIVE_${_aloe_dpdk_library}" _aloe_dpdk_archive_variable)
        find_library(${_aloe_dpdk_archive_variable}
                NAMES ${_aloe_dpdk_library}
                PATHS ${ALOE_DPDK_STATIC_LIBRARY_DIRS}
                NO_DEFAULT_PATH
                REQUIRED
        )
        mark_as_advanced(${_aloe_dpdk_archive_variable})
        list(APPEND _aloe_dpdk_archives "${${_aloe_dpdk_archive_variable}}")
    else ()
        list(APPEND _aloe_dpdk_other_libraries "${_aloe_dpdk_library}")
    endif ()
endforeach ()

list(LENGTH _aloe_dpdk_archives _aloe_dpdk_archive_count)
if (_aloe_dpdk_archive_count EQUAL 0)
    message(FATAL_ERROR
            "DPDK was found, but its link line names no static archives. Aloe links DPDK "
            "statically; check that the vcpkg triplet sets VCPKG_LIBRARY_LINKAGE to static.")
endif ()

# The wrapping is applied here, so the flags that did it on DPDK's own link line go.
set(_aloe_dpdk_link_options ${ALOE_DPDK_STATIC_LDFLAGS_OTHER})
list(REMOVE_ITEM _aloe_dpdk_link_options "-Wl,--whole-archive" "-Wl,--no-whole-archive")

# A cached DPDK tuned to the machine that built it refuses to start on another
# CPU. triplets/x64-linux-clang.cmake builds it for the generic baseline; this
# catches a triplet that lost that setting.
if ("-march=native" IN_LIST ALOE_DPDK_STATIC_CFLAGS_OTHER)
    message(FATAL_ERROR
            "DPDK was built with -march=native, which ties the vcpkg binary cache to one CPU model. "
            "Build it with -Dplatform=generic, as triplets/x64-linux-clang.cmake does.")
endif ()

list(JOIN ALOE_DPDK_STATIC_CFLAGS_OTHER " " _aloe_dpdk_compile_flags)

add_library(Aloe.Dpdk INTERFACE)
target_include_directories(Aloe.Dpdk SYSTEM INTERFACE ${ALOE_DPDK_STATIC_INCLUDE_DIRS})
target_compile_options(Aloe.Dpdk INTERFACE "SHELL:${_aloe_dpdk_compile_flags}")
target_link_directories(Aloe.Dpdk INTERFACE ${ALOE_DPDK_STATIC_LIBRARY_DIRS})
target_link_options(Aloe.Dpdk INTERFACE ${_aloe_dpdk_link_options})
target_link_libraries(Aloe.Dpdk INTERFACE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_aloe_dpdk_archives}>"
        ${_aloe_dpdk_other_libraries}
        Threads::Threads
)
add_library(Aloe::Dpdk ALIAS Aloe.Dpdk)

message("DPDK archives linked whole: ${_aloe_dpdk_archive_count}")
message("DPDK compile flags:         ${_aloe_dpdk_compile_flags}")
message("DPDK other libraries:       ${_aloe_dpdk_other_libraries}")
