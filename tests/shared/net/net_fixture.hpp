#pragma once

#include <algorithm>
#include <aloe/fabric>
#include <aloe/loop>
#include <aloe/net>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <frames.hpp>
#include <gtest/gtest.h>
#include <net_frames.hpp>

namespace aloe::testing {

    inline constexpr device::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
    inline constexpr device::MacAddress harness_mac{0x02, 0, 0, 0, 0, 0x02};
    inline constexpr device::MacAddress gateway_mac{0x02, 0, 0, 0, 0, 0xfe};
    inline constexpr device::Ipv4Address stack_ip{10, 0, 0, 2};
    inline constexpr device::Ipv4Address harness_ip{10, 0, 0, 1};
    inline constexpr device::Ipv4Address gateway_ip{10, 0, 0, 254};
    inline constexpr device::Ipv4Address far_ip{192, 168, 7, 7};  ///< Off the subnet: reached through the gateway.

    /// The brick's config in the fixture: a /24 with a gateway.
    [[nodiscard]] inline net::Ipv4Config stack_config() {
        return {.address = stack_ip, .prefix = 24, .gateway = gateway_ip};
    }

    /// The parameter's name inside a test's name.
    [[nodiscard]] inline std::string offloads_name(const ::testing::TestParamInfo<fabric::EmulatedOffloads>& info) {
        return info.param == fabric::EmulatedOffloads::Checksums ? "Checksums" : "NoOffloads";
    }

    /**
     * @brief A fabric with a harness port and a stack port, and the brick over queue 0 of the stack port.
     *
     * Parameterized over the fabric's emulated offloads, so every test runs the software and the
     * offload checksum paths. Time is `now_`: the epoch, plus whatever a test adds.
     */
    class NetFixture : public ::testing::TestWithParam<fabric::EmulatedOffloads> {
    protected:
        using Packet    = fabric::Packet;
        using Ipv4      = net::Ipv4<fabric::Port>;
        using TimePoint = aloe::core::TimePoint;

        static constexpr std::size_t ring_capacity = 16;

        fabric::Fabric fabric_;
        fabric::Port& harness_ = fabric_.add_port({.mac = harness_mac, .pool_size = 64, .offloads = GetParam()});
        fabric::Port& port_    = fabric_.add_port({.mac = stack_mac, .pool_size = 64, .offloads = GetParam()});
        loop::ShardCounters counters_;
        loop::ShardQueue<fabric::Port> queue_{port_, 0, ring_capacity, counters_};
        Ipv4 ip_{queue_, stack_config()};
        TimePoint now_{};
        std::vector<Packet> burst_ = std::vector<Packet>(64);
        std::size_t last_burst_ = 0;  ///< How many packets the last `inject`, `local` or `process_pending` handed over.

        [[nodiscard]] static bool offloads() {
            return GetParam() == fabric::EmulatedOffloads::Checksums;
        }

        /// Sends `frame` from the harness, then runs one `process` over what the stack port received.
        void inject(const std::span<const std::byte> frame) {
            auto packet = harness_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_.transmit(0, out), 1);
            process_pending();
        }

        /// Runs one `process` over whatever the stack port has received, which may be nothing.
        void process_pending() {
            last_burst_ = port_.receive(0, burst_);
            ip_.process(std::span<Packet>{burst_}.first(last_burst_), now_);
        }

        /// Builds `frame` on the stack port's own pool and hands it to `process`: for frames the fabric would not
        /// deliver.
        void local(const std::span<const std::byte> frame) {
            auto packet = port_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(fill(*packet, frame));
            burst_[0]   = std::move(*packet);
            last_burst_ = 1;
            ip_.process(std::span<Packet>{burst_}.first(1), now_);
        }

        /// Flushes the brick's queue and returns every frame the harness has received, oldest first.
        [[nodiscard]] std::vector<std::vector<std::byte>> harness_received() {
            std::ignore = queue_.flush();
            std::vector<std::vector<std::byte>> frames;
            std::array<Packet, 16> out;
            for (std::size_t count = harness_.receive(0, out); count > 0; count = harness_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(bytes_of(out[index]));
                    out[index] = Packet{};
                }
            }
            return frames;
        }

        /// Every slot the last `process` was handed is empty afterwards.
        [[nodiscard]] bool burst_empty() const {
            return std::ranges::all_of(std::span<const Packet>{burst_}.first(last_burst_),
                                       [](const Packet& packet) { return packet.empty(); });
        }
    };

}  // namespace aloe::testing
