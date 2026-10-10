set(FRAMES ${FIXTURES}.Frames)

##############################################################################
# Frame builders and a parser: Ethernet, IPv4, ARP and ICMP frames as bytes, and packets filled and read back
##############################################################################
add_library(${FRAMES}.Builders INTERFACE
        frames/frames.hpp
        frames/net_frames.hpp
        frames/tcp_frames.hpp
        frames/tcp_peer.hpp
)
target_include_directories(${FRAMES}.Builders INTERFACE
        frames/
)
target_link_libraries(${FRAMES}.Builders INTERFACE
        Aloe::Common::Device
        Aloe::Common::Wire
)
##############################################################################

##############################################################################
# Frames exported library
##############################################################################
add_combined_library(${FRAMES}
        DIRECTORIES
        export
        SOURCES
        export/aloe/frames
        LIBRARIES
        ${FRAMES}.Builders
)

add_library(Aloe::Fixtures::Frames ALIAS ${FRAMES})
##############################################################################
