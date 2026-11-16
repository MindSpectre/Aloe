set(NET ${COMMON}.Net)

##############################################################################
# The ARP cache
##############################################################################
add_library(${NET}.Arp STATIC
        ipv4/arp/arp_cache.hpp
        ipv4/arp/arp_cache.cpp
        ipv4/arp/arp_resolution.hpp
)
target_include_directories(${NET}.Arp PUBLIC
        ipv4/arp/
)
target_link_libraries(${NET}.Arp PUBLIC
        Aloe::Common::Wire
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# The brick: IPv4 over a shard queue, with ARP and ICMP echo
##############################################################################
add_library(${NET}.Stack INTERFACE
        ipv4/stack/detail/ipv4_arp.hpp
        ipv4/stack/detail/ipv4_input.hpp
        ipv4/stack/detail/ipv4_output.hpp
        ipv4/stack/detail/ipv4_routing.hpp
        ipv4/stack/detail/ipv4_transmit.hpp
        ipv4/stack/ipv4.hpp
        ipv4/stack/ipv4_config.hpp
        ipv4/stack/ipv4_counters.hpp
        ipv4/stack/ipv4_datagram.hpp
        ipv4/stack/ipv4_prebuilt.hpp
)
target_include_directories(${NET}.Stack INTERFACE
        ipv4/stack/
)
target_link_libraries(${NET}.Stack INTERFACE
        Aloe::Common::Wire
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
        ${NET}.Arp
        ${NET}.Stack
)

add_library(Aloe::Common::Net ALIAS ${NET})
##############################################################################
