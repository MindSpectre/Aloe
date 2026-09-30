#include <aloe/ethdev>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <eal_environment.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// Needs CAP_NET_ADMIN: DPDK's tap driver creates a kernel interface. Run it as
// root, by hand:
//
//     sudo ./build/debug/tests/manual_tests/ethdev/Aloe.Tests.Manual.Ethdev.Tap
//
namespace {

    constexpr std::string_view interface = "aloe-test";
    constexpr aloe::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};
    constexpr std::chrono::milliseconds patience{2000};

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-test"});

    /// A raw packet socket bound to one interface: the kernel's end of the tap.
    class PacketSocket {
    public:
        explicit PacketSocket(std::string_view name)
            : fd_{socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))} {
            if (fd_ < 0) {
                throw std::runtime_error{std::string{"socket(AF_PACKET): "} + std::strerror(errno)};
            }
            index_ = static_cast<int>(if_nametoindex(std::string{name}.c_str()));
            if (index_ == 0) {
                close(fd_);
                throw std::runtime_error{"no interface named " + std::string{name}};
            }
            sockaddr_ll address{};
            address.sll_family   = AF_PACKET;
            address.sll_protocol = htons(ETH_P_ALL);
            address.sll_ifindex  = index_;
            if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
                const int error = errno;
                close(fd_);
                throw std::runtime_error{std::string{"bind: "} + std::strerror(error)};
            }
        }

        PacketSocket(const PacketSocket&)            = delete;
        PacketSocket& operator=(const PacketSocket&) = delete;

        ~PacketSocket() {
            close(fd_);
        }

        void send(std::span<const std::byte> frame) const {
            sockaddr_ll address{};
            address.sll_family  = AF_PACKET;
            address.sll_ifindex = index_;
            address.sll_halen   = aloe::MacAddress::size;
            std::memcpy(address.sll_addr, frame.data(), aloe::MacAddress::size);
            const ssize_t sent = sendto(
                fd_, frame.data(), frame.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            ASSERT_EQ(sent, static_cast<ssize_t>(frame.size())) << std::strerror(errno);
        }

        /// The next frame within `timeout`, or nothing.
        [[nodiscard]] std::optional<std::vector<std::byte>> receive(std::chrono::milliseconds timeout) const {
            pollfd waiting{.fd = fd_, .events = POLLIN, .revents = 0};
            if (poll(&waiting, 1, static_cast<int>(timeout.count())) <= 0) {
                return std::nullopt;
            }
            std::vector<std::byte> frame(2048);
            const ssize_t got = recv(fd_, frame.data(), frame.size(), 0);
            if (got < 0) {
                return std::nullopt;
            }
            frame.resize(static_cast<std::size_t>(got));
            return frame;
        }

    private:
        int fd_;
        int index_ = 0;
    };

    /// Polls the port until `wanted` arrives or `patience` runs out; other frames, such as the
    /// kernel's IPv6 neighbour discovery, are ignored.
    [[nodiscard]] bool port_receives(aloe::ethdev::Port& port, std::span<const std::byte> wanted) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            std::array<aloe::ethdev::Packet, 8> burst;
            const std::size_t count = port.receive(0, burst);
            for (std::size_t index = 0; index < count; ++index) {
                if (aloe::testing::bytes_of(burst[index]) == std::vector<std::byte>(wanted.begin(), wanted.end())) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool kernel_receives(const PacketSocket& kernel, std::span<const std::byte> wanted) {
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
    const PacketSocket kernel{interface};

    const auto inbound = aloe::testing::ethernet_frame(
        port.mac(), peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(60, 1));
    ASSERT_NO_FATAL_FAILURE(kernel.send(inbound));
    EXPECT_TRUE(port_receives(port, inbound)) << "a frame written on the kernel side reaches the port";

    const auto outbound = aloe::testing::ethernet_frame(aloe::MacAddress::broadcast(),
                                                        port.mac(),
                                                        aloe::testing::ethertype_experimental,
                                                        aloe::testing::pattern(60, 2));
    auto packet         = port.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::testing::fill(*packet, outbound));
    std::array<aloe::ethdev::Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(port.transmit(0, burst), 1);
    EXPECT_TRUE(kernel_receives(kernel, outbound)) << "a frame transmitted by the port reaches the kernel";
}
