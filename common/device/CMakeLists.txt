set(DEVICE ${COMMON}.Device)

##############################################################################
# Receive-side scaling: the Toeplitz hash and the steering rule
##############################################################################
add_library(${DEVICE}.Steering INTERFACE
        steering/rss.hpp
)
target_include_directories(${DEVICE}.Steering INTERFACE
        steering/
)
target_link_libraries(${DEVICE}.Steering INTERFACE
        Aloe::Common::Wire
)
##############################################################################

##############################################################################
# The Packet concept and its metadata
##############################################################################
add_library(${DEVICE}.Packet INTERFACE
        packet/packet.hpp
)
target_include_directories(${DEVICE}.Packet INTERFACE
        packet/
)
##############################################################################

##############################################################################
# The Device concept, capabilities and counters
##############################################################################
add_library(${DEVICE}.Device INTERFACE
        device/device.hpp
)
target_include_directories(${DEVICE}.Device INTERFACE
        device/
)
target_link_libraries(${DEVICE}.Device INTERFACE
        ${DEVICE}.Packet
        ${DEVICE}.Steering
        Aloe::Common::Wire
)
##############################################################################

##############################################################################
# Device exported library
##############################################################################
add_combined_library(${DEVICE}
        DIRECTORIES
        export
        SOURCES
        export/aloe/device
        LIBRARIES
        ${DEVICE}.Steering
        ${DEVICE}.Packet
        ${DEVICE}.Device
)

add_library(Aloe::Common::Device ALIAS ${DEVICE})
##############################################################################
