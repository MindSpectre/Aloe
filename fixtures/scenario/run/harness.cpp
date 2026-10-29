#include "harness.hpp"

#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace aloe::scenario {

    namespace {

        constexpr std::size_t ring_capacity = 64;
        constexpr std::size_t burst_size    = 64;
        constexpr std::size_t collect_burst = 16;

    }  // namespace

    template <typename CongestionControl>
    BasicHarness<CongestionControl>::BasicHarness(const HarnessConfig& config)
        : config_{
              config
    },
          peer_port_{&fabric_.add_port({.mac         = config.peer_mac,
                                        .pool_size   = config.pool_size,
                                        .queue_depth = 4096,
                                        .offloads    = config.offloads})},
          stack_port_{
              &fabric_.add_port({.mac = config.stack_mac, .pool_size = config.pool_size, .offloads = config.offloads})},
          queue_{*stack_port_, 0, ring_capacity, counters_},
          ip_{queue_,
              net::Ipv4Config{.address       = config.stack_address,
                              .prefix        = config.prefix,
                              .arp_reachable = config.arp_lifetime,
                              .arp_expire    = 2 * config.arp_lifetime}},
          wheel_{config.resolution, core::TimePoint{}}, tcp_{ip_, wheel_, config.tcp}, burst_(burst_size) {
        ip_.learn(config.peer_address, config.peer_mac, core::TimePoint{});
    }

    template <typename CongestionControl>
    void BasicHarness<CongestionControl>::inject(const std::span<const std::byte> frame, const core::TimePoint now) {
        auto packet = peer_port_->allocate(0);
        if (!packet || !frames::fill(*packet, frame)) {
            throw std::runtime_error{"the harness could not build the peer's frame"};
        }
        std::array<Packet, 1> out{std::move(*packet)};
        if (peer_port_->transmit(0, out) != 1) {
            throw std::runtime_error{"the peer's port refused the frame"};
        }
        receive(now);
    }

    template <typename CongestionControl>
    void BasicHarness<CongestionControl>::step(const core::TimePoint now) {
        fabric_.advance(now);  // first, so a frame it delivers is received in this step
        receive(now);
        std::ignore = wheel_.advance(now);
    }

    template <typename CongestionControl>
    void BasicHarness<CongestionControl>::receive(const core::TimePoint now) {
        const std::size_t count = queue_.receive(burst_);
        ip_.process(std::span<Packet>{burst_}.first(count), now);
        tcp_.process(ip_.received(wire::Ipv4Protocol::Tcp), now);
    }

    template <typename CongestionControl>
    void BasicHarness<CongestionControl>::flush(const core::TimePoint now) {
        tcp_.flush(now);
        std::ignore = queue_.flush();
    }

    template <typename CongestionControl>
    std::vector<Collected> BasicHarness<CongestionControl>::collect() {
        std::vector<Collected> collected;
        std::array<Packet, collect_burst> out;
        for (std::size_t count = peer_port_->receive(0, out); count > 0; count = peer_port_->receive(0, out)) {
            for (std::size_t index = 0; index < count; ++index) {
                if (auto parsed = frames::parse_frame(frames::bytes_of(out[index]))) {
                    Collected item{.frame = std::move(*parsed)};
                    item.ipv4_checksum_ok =
                        item.frame.ipv4.has_value() && wire::internet_checksum(item.frame.ipv4_header) == 0;
                    item.tcp_checksum_ok = item.frame.ipv4.has_value() && item.frame.tcp.has_value() &&
                                           frames::l4_checksum_residue(*item.frame.ipv4, item.frame.l4) == 0;
                    collected.push_back(std::move(item));
                }
                out[index] = Packet{};
            }
        }
        return collected;
    }

    template class BasicHarness<tcp::NewReno>;
    template class BasicHarness<tcp::NoCongestionControl>;

    HarnessConfig configured(HarnessConfig base, const Script& script) {
        const ScriptTcpOptions& options = script.tcp;
        tcp::TcpConfig& tcp             = base.tcp;
        if (options.rto_min) {
            tcp.rto_min = *options.rto_min;
        }
        if (options.rto_max) {
            tcp.rto_max = *options.rto_max;
        }
        if (options.rto_initial) {
            tcp.rto_initial = *options.rto_initial;
        }
        if (options.user_timeout) {
            tcp.user_timeout = *options.user_timeout;
        }
        if (options.retries) {
            tcp.retries = *options.retries;
        }
        if (options.data_retries) {
            tcp.data_retries = *options.data_retries;
        }
        if (options.retransmit_defers) {
            tcp.retransmit_defers = *options.retransmit_defers;
        }
        if (options.send_segments) {
            tcp.send_segments = *options.send_segments;
        }
        if (options.timestamps) {
            tcp.timestamps = *options.timestamps;
        }
        return base;
    }

}  // namespace aloe::scenario
