#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace aloe::tcp {

    struct NewReno;

    template <typename Ip, typename CongestionControl>
    class Connection;

    template <typename Ip, typename CongestionControl>
    struct ConnectionEvent;

    template <typename Ip, typename CongestionControl = NewReno>
    class Stack;

    /// What the brick asks of the IP layer below it: `net::Ipv4<Device>`, or anything with these members.
    template <typename Ip>
    concept IsIp =
        device::IsDevice<typename Ip::Device> && std::same_as<typename Ip::Packet, typename Ip::Device::Packet> &&
        requires(Ip& ip,
                 const Ip& const_ip,
                 typename Ip::Packet packet,
                 typename Ip::Packet& packet_ref,
                 const net::SendRequest& request,
                 core::TimePoint now,
                 wire::Ipv4Address address,
                 wire::Ipv4Protocol protocol) {
            { ip.allocate() } -> std::same_as<std::optional<typename Ip::Packet>>;
            { ip.send(std::move(packet), request, now) } -> std::same_as<std::expected<void, net::SendError>>;
            { const_ip.address() } -> std::same_as<wire::Ipv4Address>;
            { const_ip.max_l4_size() } -> std::same_as<std::uint16_t>;
            { const_ip.next_hop(address) } -> std::same_as<std::optional<wire::Ipv4Address>>;
            { ip.queue() } -> std::same_as<loop::ShardQueue<typename Ip::Device>&>;
            { const_ip.generation() } -> std::same_as<std::uint64_t>;
            { ip.prebuild(address, protocol, now) } -> std::same_as<std::expected<net::Prebuilt, net::SendError>>;
            { ip.send_frame(packet_ref, request, now) } -> std::same_as<std::expected<void, net::SendError>>;
        };

}  // namespace aloe::tcp
