set(STREAM ${COMMON}.Stream)

##############################################################################
# The stream contract: concepts, events, errors and the convenience send
##############################################################################
add_library(${STREAM}.Contract INTERFACE
        stream/stream.hpp
        stream/stream_events.hpp
)
target_include_directories(${STREAM}.Contract INTERFACE
        stream/
)
target_link_libraries(${STREAM}.Contract INTERFACE
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# Stream exported library
##############################################################################
add_combined_library(${STREAM}
        DIRECTORIES
        export
        SOURCES
        export/aloe/stream
        LIBRARIES
        ${STREAM}.Contract
)

add_library(Aloe::Common::Stream ALIAS ${STREAM})
##############################################################################
