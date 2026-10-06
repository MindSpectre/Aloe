set(FABRIC ${FIXTURES}.Fabric)

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
        Aloe::Common::Device
)
##############################################################################

##############################################################################
# Ports and delivery
##############################################################################
add_library(${FABRIC}.Fabric STATIC
        fabric/fabric.hpp
        fabric/fabric.cpp
        fabric/detail/frame.hpp
)
target_include_directories(${FABRIC}.Fabric PUBLIC
        fabric/
)
target_link_libraries(${FABRIC}.Fabric PUBLIC
        ${FABRIC}.Packet
        Threads::Threads
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
        ${FABRIC}.Fabric
)

add_library(Aloe::Fixtures::Fabric ALIAS ${FABRIC})
##############################################################################
