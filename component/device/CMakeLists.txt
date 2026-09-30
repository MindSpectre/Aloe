set(DEVICE ${COMPONENT}.Device)

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
        Aloe::Common::Core
)

add_library(Aloe::Component::Device ALIAS ${DEVICE})
##############################################################################
