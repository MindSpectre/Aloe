set(WIRE ${COMMON}.Wire)

##############################################################################
# Wire: the module aloe.wire, one partition per former header, in the folder
# the header was in.
#   bytes, checksum: big-endian loads and stores, and the Internet checksum.
#   ethernet: the MAC address and the Ethernet header.
#   ipv4: the address, the protocol numbers, the header and its checksums.
#   arp: requests and replies for Ethernet over IPv4.
#   icmp: the header of an echo request and reply.
#   tcp: sequence arithmetic, the header and the options the brick reads.
# The umbrella export/aloe/wire imports it.
##############################################################################
add_library(${WIRE} STATIC
        export/aloe/wire
)
target_sources(${WIRE} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        wire.cppm
        bytes/bytes.cppm
        checksum/checksum.cppm
        ethernet/mac_address.cppm
        ethernet/ethernet.cppm
        ipv4/ipv4_address.cppm
        ipv4/ipv4_subnet.cppm
        ipv4/ipv4_protocol.cppm
        ipv4/ipv4_header.cppm
        ipv4/ipv4_checksum.cppm
        arp/arp.cppm
        icmp/icmp.cppm
        tcp/tcp_sequence.cppm
        tcp/tcp_header.cppm
        tcp/tcp_options.cppm
)
target_include_directories(${WIRE} PUBLIC
        export
)

add_library(Aloe::Common::Wire ALIAS ${WIRE})
##############################################################################
