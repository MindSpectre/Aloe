#include <algorithm>
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <eal_environment.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Needs CAP_NET_ADMIN and CAP_NET_RAW: DPDK's tap driver creates a kernel interface, the test gives it an
// address, and a raw ICMP socket pings the brick through the kernel's own ARP and ICMP. Run it as root:
//
//     sudo ./build/debug/tests/manual_tests/net/Aloe.Tests.Manual.Net.Tap
//
namespace {

    using namespace std::chrono_literals;

    constexpr std::string_view interface = "aloe-ping";
    constexpr aloe::device::Ipv4Address kernel_ip{10, 77, 0, 1};
    constexpr aloe::device::Ipv4Address stack_ip{10, 77, 0, 2};
    constexpr aloe::device::Ipv4Address netmask{255, 255, 255, 0};
    constexpr std::uint16_t identifier = 0xa10e;
    constexpr auto patience            = 2000ms;

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-ping"});

    /// Gives the kernel end of the tap an address and brings it up: `ip addr add` and `ip link set up`, by ioctl.
    void configure_interface(const std::string_view name,
                             const aloe::device::Ipv4Address address,
                             const aloe::device::Ipv4Address mask) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(fd, 0) << std::strerror(errno);
        ifreq request{};
        std::strncpy(request.ifr_name, std::string{name}.c_str(), IFNAMSIZ - 1);
        auto& in      = *reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
        in.sin_family = AF_INET;
        std::memcpy(&in.sin_addr, address.bytes().data(), aloe::device::Ipv4Address::size);
        EXPECT_EQ(ioctl(fd, SIOCSIFADDR, &request), 0) << "SIOCSIFADDR: " << std::strerror(errno);
        std::memcpy(&in.sin_addr, mask.bytes().data(), aloe::device::Ipv4Address::size);
        EXPECT_EQ(ioctl(fd, SIOCSIFNETMASK, &request), 0) << "SIOCSIFNETMASK: " << std::strerror(errno);
        EXPECT_EQ(ioctl(fd, SIOCGIFFLAGS, &request), 0) << "SIOCGIFFLAGS: " << std::strerror(errno);
        request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
        EXPECT_EQ(ioctl(fd, SIOCSIFFLAGS, &request), 0) << "SIOCSIFFLAGS: " << std::strerror(errno);
        close(fd);
    }

    /// The brick on a hand-written loop over one queue, on its own thread: the loop the docs lead with.
    class PingLoop {
    public:
        explicit PingLoop(aloe::ethdev::Port& port)
            : thread_{[this, &port](const std::stop_token& stop) { run(port, stop); }} {
        }

        PingLoop(const PingLoop&)            = delete;
        PingLoop& operator=(const PingLoop&) = delete;
        PingLoop(PingLoop&&)                 = delete;
        PingLoop& operator=(PingLoop&&)      = delete;

        ~PingLoop() {
            stop();
        }

        void stop() {
            if (thread_.joinable()) {
                thread_.request_stop();
                thread_.join();
            }
        }

        /// Read after `stop`.
        [[nodiscard]] const aloe::net::Ipv4Counters& counters() const noexcept {
            return counters_;
        }

    private:
        void run(aloe::ethdev::Port& port, const std::stop_token& stop) {
            aloe::ethdev::register_thread();
            aloe::loop::ShardCounters shard_counters;
            aloe::loop::ShardQueue<aloe::ethdev::Port> queue{port, 0, 64, shard_counters};
            aloe::net::Ipv4<aloe::ethdev::Port> ip{
                queue, {.address = stack_ip, .prefix = 24}
            };
            std::vector<aloe::ethdev::Packet> burst(64);
            while (!stop.stop_requested()) {
                const auto now             = std::chrono::steady_clock::now();
                const std::size_t received = queue.receive(burst);
                ip.process(std::span<aloe::ethdev::Packet>{burst}.first(received), now);
                std::ignore = queue.flush();
                if (received == 0) {
                    std::this_thread::yield();
                }
            }
            counters_ = ip.counters();
            queue.discard();
        }

        aloe::net::Ipv4Counters counters_{};
        std::jthread thread_;  ///< Last, so it starts after everything it reads exists.
    };

    /// An echo request as `ping` sends it: 8-byte header, 56 bytes of payload, our identifier, sequence 1.
    [[nodiscard]] std::array<std::byte, 64> echo_request(const std::span<const std::byte> payload) {
        std::array<std::byte, 64> message{};
        aloe::net::write_icmp(message,
                              {.type     = aloe::net::IcmpType::EchoRequest,
                               .code     = 0,
                               .checksum = 0,
                               .rest     = (static_cast<std::uint32_t>(identifier) << 16U) | 1U});
        std::ranges::copy(payload, message.begin() + static_cast<std::ptrdiff_t>(aloe::net::icmp_header_size));
        aloe::device::store_be16(std::span<std::byte>{message}.subspan(aloe::net::icmp_checksum_offset, 2),
                                 aloe::device::internet_checksum(message));
        return message;
    }

}  // namespace

TEST(NetTap, TheKernelPingsTheBrick) {
    if (geteuid() != 0) {
        FAIL() << "this test creates a tap interface and pings it through the kernel; run it as root";
    }
    aloe::ethdev::Port port{
        {.name = "net_tap0", .queues = 1, .pool_size = 512}
    };
    configure_interface(interface, kernel_ip, netmask);
    PingLoop loop{port};

    const int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    const auto payload = aloe::testing::pattern(56);
    const auto message = echo_request(payload);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    std::memcpy(&to.sin_addr, stack_ip.bytes().data(), aloe::device::Ipv4Address::size);
    const ssize_t sent =
        sendto(fd, message.data(), message.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    ASSERT_EQ(sent, static_cast<ssize_t>(message.size())) << std::strerror(errno);

    // The kernel resolves 10.77.0.2 by ARP first, then sends the echo. The raw socket hands back the IPv4
    // header too, and every ICMP the host receives, so match our identifier.
    bool answered       = false;
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (!answered && std::chrono::steady_clock::now() < deadline) {
        pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (poll(&waiting, 1, static_cast<int>(std::max<std::int64_t>(left.count(), 1))) <= 0) {
            break;
        }
        std::array<std::byte, 2048> buffer{};
        const ssize_t got = recv(fd, buffer.data(), buffer.size(), 0);
        if (got < 0) {
            break;
        }
        const std::span<const std::byte> datagram{buffer.data(), static_cast<std::size_t>(got)};
        const auto ip = aloe::net::parse_ipv4(datagram);
        if (!ip || ip->source != stack_ip || ip->protocol != aloe::device::Ipv4Protocol::Icmp) {
            continue;
        }
        const std::span<const std::byte> reply =
            datagram.subspan(ip->header_length, ip->total_length - ip->header_length);
        const auto icmp = aloe::net::parse_icmp(reply);
        if (!icmp || icmp->type != aloe::net::IcmpType::EchoReply || (icmp->rest >> 16U) != identifier) {
            continue;
        }
        EXPECT_EQ(icmp->rest & 0xffffU, 1U);
        EXPECT_EQ(aloe::device::internet_checksum(reply), 0);
        EXPECT_EQ(std::vector<std::byte>(reply.begin() + 8, reply.end()), payload);
        answered = true;
    }
    close(fd);
    EXPECT_TRUE(answered) << "no echo reply within " << patience.count() << " ms";

    loop.stop();
    EXPECT_GE(loop.counters().arp_replies_sent, 1U) << "the kernel asked who has " << stack_ip;
    EXPECT_GE(loop.counters().echo_replies_sent, 1U);
}
