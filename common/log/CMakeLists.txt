set(LOG ${COMMON}.Log)

##############################################################################
# Log: the module aloe.log, the one place that names quill. One interface unit,
# log.cppm: loggers, levels and the Logging backend owner. The umbrella
# export/aloe/log imports it.
##############################################################################
add_library(${LOG} STATIC
        export/aloe/log
)
target_sources(${LOG} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        log.cppm
)
target_include_directories(${LOG} PUBLIC
        export
)
target_link_libraries(${LOG} PUBLIC
        quill::quill
        Aloe::Common::Core
        Threads::Threads
)

add_library(Aloe::Common::Log ALIAS ${LOG})
##############################################################################
