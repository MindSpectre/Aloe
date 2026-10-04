set(DEVICE ${COMMON}.Device)

##############################################################################
# Addresses: MAC and IPv4 value types
##############################################################################
add_library(${DEVICE}.Address INTERFACE
        address/address.hpp
)
target_include_directories(${DEVICE}.Address INTERFACE
        address/
)
##############################################################################

##############################################################################
# Byte order helpers
##############################################################################
add_library(${DEVICE}.Bytes INTERFACE
        bytes/bytes.hpp
)
target_include_directories(${DEVICE}.Bytes INTERFACE
        bytes/
)
##############################################################################

##############################################################################
# The IPv4 protocol number as a type
##############################################################################
add_library(${DEVICE}.Protocol INTERFACE
        protocol/protocol.hpp
)
target_include_directories(${DEVICE}.Protocol INTERFACE
        protocol/
)
##############################################################################

##############################################################################
# Internet checksum
##############################################################################
add_library(${DEVICE}.Checksum INTERFACE
        checksum/checksum.hpp
)
target_include_directories(${DEVICE}.Checksum INTERFACE
        checksum/
)
target_link_libraries(${DEVICE}.Checksum INTERFACE
        ${DEVICE}.Address
        ${DEVICE}.Bytes
        ${DEVICE}.Protocol
)
##############################################################################

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
        ${DEVICE}.Address
        ${DEVICE}.Bytes
        ${DEVICE}.Protocol
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
        ${DEVICE}.Address
        ${DEVICE}.Packet
        ${DEVICE}.Steering
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
        ${DEVICE}.Address
        ${DEVICE}.Bytes
        ${DEVICE}.Protocol
        ${DEVICE}.Checksum
        ${DEVICE}.Steering
        ${DEVICE}.Packet
        ${DEVICE}.Device
        Aloe::Common::Core
)

add_library(Aloe::Common::Device ALIAS ${DEVICE})
##############################################################################
