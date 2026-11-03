##############################################################################
# The TCP brick over the ring driver: a scripted peer on real mbufs, no root
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Tcp.Ring
        test_tcp_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Tcp.Ring
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Ethdev
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Ethdev
        # Tcp, Net, Ethdev and the shared test libraries are INTERFACE libraries that forward converted modules. CMake 4.4
        # gives a target the plain BMIs of a module it reaches only through an INTERFACE library, beside the synthesized
        # ones of the modules it links directly, and clang cannot use the mix (ledger 10). Linked directly, they get
        # synthesized BMIs too.
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Loop
        Aloe::Common::Stream
        Aloe::Common::Wire
        ${TEST_LIBS}
)
##############################################################################
