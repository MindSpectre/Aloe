set(FABRIC ${COMPONENT}.Fabric)

##############################################################################
# The fabric's packet and pool
##############################################################################
add_library(${FABRIC}.Packet STATIC
        packet/fabric_packet.hpp
        packet/fabric_packet.cpp
)
target_include_directories(${FABRIC}.Packet PUBLIC
        packet/
)
target_link_libraries(${FABRIC}.Packet PUBLIC
        Aloe::Component::Device
)
##############################################################################

##############################################################################
# Fabric exported library
##############################################################################
add_combined_library(${FABRIC}
        DIRECTORIES
        export
        SOURCES
        export/aloe/fabric
        LIBRARIES
        ${FABRIC}.Packet
)

add_library(Aloe::Component::Fabric ALIAS ${FABRIC})
##############################################################################
