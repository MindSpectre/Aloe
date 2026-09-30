set(ETHDEV ${COMPONENT}.Ethdev)

##############################################################################
# The environment abstraction layer, started once per process
##############################################################################
add_library(${ETHDEV}.Eal STATIC
        eal/eal.hpp
        eal/eal.cpp
)
target_include_directories(${ETHDEV}.Eal PUBLIC
        eal/
)
target_link_libraries(${ETHDEV}.Eal PUBLIC
        Aloe::Dpdk
)
##############################################################################

##############################################################################
# The mbuf-backed packet
##############################################################################
add_library(${ETHDEV}.Packet INTERFACE
        packet/ethdev_packet.hpp
)
target_include_directories(${ETHDEV}.Packet INTERFACE
        packet/
)
target_link_libraries(${ETHDEV}.Packet INTERFACE
        Aloe::Component::Device
        Aloe::Dpdk
)
##############################################################################

##############################################################################
# Ethdev exported library
##############################################################################
add_combined_library(${ETHDEV}
        DIRECTORIES
        export
        SOURCES
        export/aloe/ethdev
        LIBRARIES
        ${ETHDEV}.Eal
        ${ETHDEV}.Packet
)

add_library(Aloe::Component::Ethdev ALIAS ${ETHDEV})
##############################################################################
