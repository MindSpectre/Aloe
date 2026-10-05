set(NET ${COMMON}.Net)

##############################################################################
# Wire formats: Ethernet, ARP, IPv4 and ICMP headers as functions over bytes
##############################################################################
add_library(${NET}.Wire INTERFACE
        wire/ethernet.hpp
        wire/arp.hpp
        wire/ipv4_header.hpp
        wire/icmp.hpp
)
target_include_directories(${NET}.Wire INTERFACE
        wire/
)
target_link_libraries(${NET}.Wire INTERFACE
        Aloe::Common::Device
)
##############################################################################

##############################################################################
# The ARP cache: a fixed table aged against the caller's stamp
##############################################################################
add_library(${NET}.Arp STATIC
        arp/arp_cache.hpp
        arp/arp_cache.cpp
)
target_include_directories(${NET}.Arp PUBLIC
        arp/
)
target_link_libraries(${NET}.Arp PUBLIC
        Aloe::Common::Device
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# The brick: IPv4 over a shard queue, with ARP and ICMP echo
##############################################################################
add_library(${NET}.Stack INTERFACE
        stack/datagram.hpp
        stack/ipv4.hpp
)
target_include_directories(${NET}.Stack INTERFACE
        stack/
)
target_link_libraries(${NET}.Stack INTERFACE
        ${NET}.Wire
        ${NET}.Arp
        Aloe::Common::Loop
        Aloe::Common::Device
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# Net exported library
##############################################################################
add_combined_library(${NET}
        DIRECTORIES
        export
        SOURCES
        export/aloe/net
        LIBRARIES
        ${NET}.Wire
        ${NET}.Arp
        ${NET}.Stack
)

add_library(Aloe::Common::Net ALIAS ${NET})
##############################################################################
