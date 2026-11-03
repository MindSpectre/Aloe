set(FABRIC ${FIXTURES}.Fabric)

##############################################################################
# Fabric: the module aloe.fabric, one partition per former header, in the
# folder the header was in.
#   fabric_packet: the packet and its pool.
#   impairment: the scripted and modelled loss, duplication, reorder and delay.
#   frame: what the fabric parses of an IPv4 frame (was detail/frame.hpp).
#   fabric: the ports and the fabric that delivers between them.
# The umbrella export/aloe/fabric imports it.
##############################################################################
add_library(${FABRIC} STATIC
        fabric/fabric.cpp
        packet/fabric_packet.cpp
        export/aloe/fabric
)
target_sources(${FABRIC} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        fabric.cppm
        fabric/fabric.cppm
        fabric/impairment.cppm
        fabric/detail/frame.cppm
        packet/fabric_packet.cppm
)
target_include_directories(${FABRIC} PUBLIC
        export
)
target_link_libraries(${FABRIC} PUBLIC
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Wire
        Threads::Threads
)

add_library(Aloe::Fixtures::Fabric ALIAS ${FABRIC})
##############################################################################
