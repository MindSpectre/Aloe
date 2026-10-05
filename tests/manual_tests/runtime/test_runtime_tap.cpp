#include <algorithm>
#include <aloe/ethdev>
#include <aloe/runtime>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <eal_environment.hpp>
#include <echo_stack.hpp>
#include <frames.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>
#include <packet_socket.hpp>
#include <unistd.h>

// Needs CAP_NET_ADMIN: DPDK's tap driver creates a kernel interface. Run it as root, by hand:
//
//     sudo ./build/debug/tests/manual_tests/runtime/Aloe.Tests.Manual.Runtime.Tap
//
namespace {

    using namespace std::chrono_literals;

    constexpr std::string_view interface = "aloe-echo";
    constexpr aloe::wire::MacAddress peer{0x02, 0, 0, 0, 0xfe, 0xed};
    constexpr auto patience = 2000ms;

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-echo"});
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    /// What the echo makes of `frame`: addresses swapped, shard 0 stamped into the last two bytes.
    [[nodiscard]] std::vector<std::byte> echo_of(std::vector<std::byte> frame) {
        std::swap_ranges(frame.begin(), frame.begin() + 6, frame.begin() + 6);
        aloe::wire::store_be16(std::span<std::byte>{frame}.last(2), 0);
        return frame;
    }

}  // namespace

TEST(RuntimeTap, TheKernelGetsItsFramesBackWithTheAddressesSwapped) {
    if (geteuid() != 0) {
        FAIL() << "this test creates a tap interface and needs CAP_NET_ADMIN; run it as root";
    }
    aloe::ethdev::Port port{
        {.name = "net_tap0", .queues = 1, .pool_size = 512}
    };
    aloe::runtime::Runtime<aloe::ethdev::Port, aloe::testing::EchoStack<aloe::ethdev::Port>> runtime{
        {.shard       = {.idle = aloe::runtime::IdlePolicy::Yield, .yield_after = 100},
         .threads     = {},
         .thread_hook = aloe::ethdev::register_thread},
        port
    };
    runtime.start();
    const aloe::testing::PacketSocket kernel{interface};

    for (std::uint8_t round = 0; round < 3; ++round) {
        const auto frame = aloe::testing::ethernet_frame(
            port.mac(), peer, aloe::testing::ethertype_experimental, aloe::testing::pattern(60, round));
        ASSERT_TRUE(kernel.send(frame));
        const auto expected = echo_of(frame);
        bool answered       = false;
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!answered && std::chrono::steady_clock::now() < deadline) {
            const auto reply = kernel.receive(patience);  // the kernel also sends its own traffic; skip it
            answered         = reply.has_value() && *reply == expected;
        }
        EXPECT_TRUE(answered) << "round " << round;
    }

    runtime.stop();
    runtime.join();
    EXPECT_GE(runtime.counters(0).frames_transmitted, 3U);
}
