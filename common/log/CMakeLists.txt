set(LOG ${COMMON}.Log)

##############################################################################
# Logging: the one place that names quill
##############################################################################
add_library(${LOG}.Logger INTERFACE
        log/log.hpp
)
target_include_directories(${LOG}.Logger INTERFACE
        log/
)
target_link_libraries(${LOG}.Logger INTERFACE
        quill::quill
        Aloe::Common::Core
        Threads::Threads
)
##############################################################################

##############################################################################
# Log exported library
##############################################################################
add_combined_library(${LOG}
        DIRECTORIES
        export
        SOURCES
        export/aloe/log
        LIBRARIES
        ${LOG}.Logger
)

add_library(Aloe::Common::Log ALIAS ${LOG})
##############################################################################
