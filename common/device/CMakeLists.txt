set(DEVICE ${COMMON}.Device)

##############################################################################
# Device: the module aloe.device, one partition per former header, in the
# folder the header was in.
#   packet: the Packet concept and its metadata.
#   steering: receive-side scaling, the Toeplitz hash and the steering rule.
#   device: the Device concept, capabilities and counters.
# The umbrella export/aloe/device imports it.
##############################################################################
add_library(${DEVICE} STATIC
        export/aloe/device
)
target_sources(${DEVICE} PUBLIC
        FILE_SET modules TYPE CXX_MODULES
        BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
        device.cppm
        device/device.cppm
        packet/packet.cppm
        steering/rss.cppm
)
target_include_directories(${DEVICE} PUBLIC
        export
)
target_link_libraries(${DEVICE} PUBLIC
        Aloe::Common::Wire
)

add_library(Aloe::Common::Device ALIAS ${DEVICE})
##############################################################################
