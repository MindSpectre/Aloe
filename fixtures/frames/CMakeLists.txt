set(FRAMES ${FIXTURES}.Frames)

##############################################################################
# Frames: the module aloe.frames, one partition per former header, in the
# folder the header was in.
#   frames: Ethernet and IPv4 frame builders and the packet fill helpers.
#   net_frames: ARP and ICMP frames and the parser.
#   tcp_frames: TCP segments and their payload.
#   tcp_peer: the scripted other end of a connection.
# The umbrella export/aloe/frames imports it.
##############################################################################
add_library(${FRAMES} STATIC
        export/aloe/frames
)
target_sources(${FRAMES} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        frames.cppm
        frames/frames.cppm
        frames/net_frames.cppm
        frames/tcp_frames.cppm
        frames/tcp_peer.cppm
)
target_include_directories(${FRAMES} PUBLIC
        export
)
target_link_libraries(${FRAMES} PUBLIC
        Aloe::Common::Device
        Aloe::Common::Wire
)

add_library(Aloe::Fixtures::Frames ALIAS ${FRAMES})
##############################################################################
