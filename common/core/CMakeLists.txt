set(CORE ${COMMON}.Core)

##############################################################################
# Library version, generated from the project() version as the partition
# aloe.core:version
##############################################################################
configure_file(version/version.cppm.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cppm
        @ONLY
)
##############################################################################

##############################################################################
# Core: the module aloe.core, one partition per former header.
#   clock, tsc_scale, tsc_clock: the clock, stamp and interval types the whole
#     stack shares. The TSC clock by default, the steady clock with
#     USE_TSC_CLOCK=OFF (ALOE_USE_TSC_CLOCK, set build-wide in
#     cmake/features.cmake).
#   values: discarding values on purpose, and choosing a fallback.
#   guards: static assertions that pin a member function as mutating or as an
#     instance function.
#   fixed_string: a string literal as a template parameter.
#   version: the library version.
# The umbrella export/aloe/core imports it.
##############################################################################
add_library(${CORE} STATIC
        clock/tsc_clock.cpp
        export/aloe/core
)
target_sources(${CORE} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        FILES
        core.cppm
        clock/clock.cppm
        clock/tsc_scale.cppm
        clock/tsc_clock.cppm
        values/values.cppm
        guards/guards.cppm
        fixed_string/fixed_string.cppm
        ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cppm
)
target_include_directories(${CORE} PUBLIC
        export
)

add_library(Aloe::Common::Core ALIAS ${CORE})
##############################################################################
