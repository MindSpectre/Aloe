set(CORE ${COMMON}.Core)

##############################################################################
# Library version, generated from the project() version
##############################################################################
configure_file(version/version.hpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp
        @ONLY
)
add_library(${CORE}.Version INTERFACE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp
)
target_include_directories(${CORE}.Version INTERFACE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/
)
##############################################################################

##############################################################################
# Clock: the clock, stamp and interval types the whole stack shares
##############################################################################
add_library(${CORE}.Clock INTERFACE
        clock/clock.hpp
)
target_include_directories(${CORE}.Clock INTERFACE
        clock/
)
##############################################################################

##############################################################################
# Values: discarding values on purpose, and choosing a fallback
##############################################################################
add_library(${CORE}.Values INTERFACE
        values/values.hpp
)
target_include_directories(${CORE}.Values INTERFACE
        values/
)
##############################################################################

##############################################################################
# Guards: static assertions that pin a member function as mutating or as an instance function
##############################################################################
add_library(${CORE}.Guards INTERFACE
        guards/guards.hpp
)
target_include_directories(${CORE}.Guards INTERFACE
        guards/
)
##############################################################################

##############################################################################
# FixedString: a string literal as a template parameter
##############################################################################
add_library(${CORE}.FixedString INTERFACE
        fixed_string/fixed_string.hpp
)
target_include_directories(${CORE}.FixedString INTERFACE
        fixed_string/
)
##############################################################################

##############################################################################
# Core exported library
##############################################################################
add_combined_library(${CORE}
        DIRECTORIES
        export
        SOURCES
        export/aloe/core
        LIBRARIES
        ${CORE}.Version
        ${CORE}.Clock
        ${CORE}.Values
        ${CORE}.Guards
        ${CORE}.FixedString
)

add_library(Aloe::Common::Core ALIAS ${CORE})
##############################################################################
