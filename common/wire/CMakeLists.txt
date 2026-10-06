set(WIRE ${COMMON}.Wire)

##############################################################################
# Bytes: big-endian loads and stores, and the Internet checksum
##############################################################################
add_library(${WIRE}.Bytes INTERFACE
        bytes/bytes.hpp
        checksum/checksum.hpp
)
target_include_directories(${WIRE}.Bytes INTERFACE
        bytes/
        checksum/
)
##############################################################################

##############################################################################
# Ethernet: the MAC address and the Ethernet header
##############################################################################
add_library(${WIRE}.Ethernet INTERFACE
        ethernet/mac_address.hpp
        ethernet/ethernet.hpp
)
target_include_directories(${WIRE}.Ethernet INTERFACE
        ethernet/
)
target_link_libraries(${WIRE}.Ethernet INTERFACE
        ${WIRE}.Bytes
)
##############################################################################

##############################################################################
# IPv4: the address, the protocol numbers, the header and its checksums
##############################################################################
add_library(${WIRE}.Ipv4 INTERFACE
        ipv4/ipv4_address.hpp
        ipv4/ipv4_subnet.hpp
        ipv4/ipv4_protocol.hpp
        ipv4/ipv4_header.hpp
        ipv4/ipv4_checksum.hpp
)
target_include_directories(${WIRE}.Ipv4 INTERFACE
        ipv4/
)
target_link_libraries(${WIRE}.Ipv4 INTERFACE
        ${WIRE}.Bytes
        ${WIRE}.Ethernet
)
##############################################################################

##############################################################################
# ARP: requests and replies for Ethernet over IPv4
##############################################################################
add_library(${WIRE}.Arp INTERFACE
        arp/arp.hpp
)
target_include_directories(${WIRE}.Arp INTERFACE
        arp/
)
target_link_libraries(${WIRE}.Arp INTERFACE
        ${WIRE}.Ethernet
        ${WIRE}.Ipv4
)
##############################################################################

##############################################################################
# ICMP: the header of an echo request and reply
##############################################################################
add_library(${WIRE}.Icmp INTERFACE
        icmp/icmp.hpp
)
target_include_directories(${WIRE}.Icmp INTERFACE
        icmp/
)
target_link_libraries(${WIRE}.Icmp INTERFACE
        ${WIRE}.Bytes
)
##############################################################################

##############################################################################
# Wire exported library
##############################################################################
add_combined_library(${WIRE}
        DIRECTORIES
        export
        SOURCES
        export/aloe/wire
        LIBRARIES
        ${WIRE}.Bytes
        ${WIRE}.Ethernet
        ${WIRE}.Ipv4
        ${WIRE}.Arp
        ${WIRE}.Icmp
)

add_library(Aloe::Common::Wire ALIAS ${WIRE})
##############################################################################
