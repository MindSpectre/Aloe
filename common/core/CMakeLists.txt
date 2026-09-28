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
# Execution facilities: the one place that names stdexec
##############################################################################
add_library(${CORE}.Execution INTERFACE
        execution/execution.hpp
)
target_include_directories(${CORE}.Execution INTERFACE
        execution/
)
target_link_libraries(${CORE}.Execution INTERFACE
        STDEXEC::stdexec
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
        ${CORE}.Execution
)

add_library(Aloe::Common::Core ALIAS ${CORE})
##############################################################################
