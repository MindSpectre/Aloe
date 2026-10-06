set(EXECUTION ${COMMON}.Execution)

##############################################################################
# Execution facilities: the one place that names stdexec
##############################################################################
add_library(${EXECUTION}.Facilities INTERFACE
        execution/execution.hpp
)
target_include_directories(${EXECUTION}.Facilities INTERFACE
        execution/
)
target_link_libraries(${EXECUTION}.Facilities INTERFACE
        STDEXEC::stdexec
)
##############################################################################

##############################################################################
# Execution exported library
##############################################################################
add_combined_library(${EXECUTION}
        DIRECTORIES
        export
        SOURCES
        export/aloe/execution
        LIBRARIES
        ${EXECUTION}.Facilities
)

add_library(Aloe::Common::Execution ALIAS ${EXECUTION})
##############################################################################
