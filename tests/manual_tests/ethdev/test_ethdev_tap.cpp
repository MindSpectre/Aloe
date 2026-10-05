#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <packet_socket.hpp>
#include <unistd.h>

// Needs CAP_NET_ADMIN: DPDK's tap driver creates a kernel interface. Run it as
// root, by hand:
//
//     sudo ./build/debug/tests/manual_tests/ethdev/Aloe.Tests.Manual.Ethdev.Tap
//
namespace {

    constexpr std::string_view interface = "aloe-test";
    constexpr aloe::wire::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};
    constexpr std::chrono::milliseconds patience{2000};

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-test"});

    /// Polls the port until `wanted` arrives or `patience` runs out; other frames, such as the
    /// kernel's IPv6 neighbour discovery, are ignored.
    [[nodiscard]] bool port_receives(aloe::ethdev::Port& port, std::span<const std::byte> wanted) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            std::array<aloe::ethdev::Packet, 8> burst;
            const std::size_t count = port.receive(0, burst);
            for (std::size_t index = 0; index < count; ++index) {
                if (aloe::frames::bytes_of(burst[index]) == std::vector<std::byte>(wanted.begin(), wanted.end())) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool kernel_receives(const aloe::testing::PacketSocket& kernel, std::span<const std::byte> wanted) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto frame = kernel.receive(patience);
            if (frame && *frame == std::vector<std::byte>(wanted.begin(), wanted.end())) {
                return true;
            }
        }
        return false;
    }

}  // namespace

TEST(EthdevTap, FramesCrossBetweenTheKernelAndThePort) {
    if (geteuid() != 0) {
        FAIL() << "this test creates a tap interface and needs CAP_NET_ADMIN; run it as root";
    }
    aloe::ethdev::Port port{
        {.name = "net_tap0", .queues = 1, .pool_size = 512}
    };
    EXPECT_EQ(port.driver_name(), "net_tap");
    const aloe::testing::PacketSocket kernel{interface};

    const auto inbound = aloe::frames::ethernet_frame(
        port.mac(), peer, aloe::frames::ethertype_experimental, aloe::frames::pattern(60, 1));
    ASSERT_TRUE(kernel.send(inbound));
    EXPECT_TRUE(port_receives(port, inbound)) << "a frame written on the kernel side reaches the port";

    const auto outbound = aloe::frames::ethernet_frame(aloe::wire::MacAddress::broadcast(),
                                                       port.mac(),
                                                       aloe::frames::ethertype_experimental,
                                                       aloe::frames::pattern(60, 2));
    auto packet         = port.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, outbound));
    std::array<aloe::ethdev::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(port.transmit(0, burst), 1);
    EXPECT_TRUE(kernel_receives(kernel, outbound)) << "a frame transmitted by the port reaches the kernel";
}
