set(ETHDEV ${COMMON}.Ethdev)

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
        Aloe::Common::Device
        Aloe::Dpdk
        Aloe::Common::Core
)
##############################################################################

##############################################################################
# Ports
##############################################################################
add_library(${ETHDEV}.Port STATIC
        port/port.hpp
        port/port.cpp
)
target_include_directories(${ETHDEV}.Port PUBLIC
        port/
)
target_link_libraries(${ETHDEV}.Port PUBLIC
        ${ETHDEV}.Eal
        ${ETHDEV}.Packet
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
        ${ETHDEV}.Port
)

add_library(Aloe::Common::Ethdev ALIAS ${ETHDEV})
##############################################################################
