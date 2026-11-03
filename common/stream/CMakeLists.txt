set(STREAM ${COMMON}.Stream)

##############################################################################
# The stream contract: the module aloe.stream, one partition per former header.
#   stream: the concepts and the convenience send.
#   stream_events: the events and errors.
# The umbrella export/aloe/stream imports it.
##############################################################################
add_library(${STREAM} STATIC
        export/aloe/stream
)
target_sources(${STREAM} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        stream.cppm
        stream/stream.cppm
        stream/stream_events.cppm
)
target_include_directories(${STREAM} PUBLIC
        export
)
target_link_libraries(${STREAM} PUBLIC
        Aloe::Common::Core
)

add_library(Aloe::Common::Stream ALIAS ${STREAM})
##############################################################################
