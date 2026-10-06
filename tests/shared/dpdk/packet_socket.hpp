#pragma once

#include <aloe/device>
#include <aloe/wire>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace aloe::testing {

    /// A raw packet socket bound to one interface: the kernel's end of a tap.
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

        [[nodiscard]] bool send(std::span<const std::byte> frame) const noexcept {
            sockaddr_ll address{};
            address.sll_family  = AF_PACKET;
            address.sll_ifindex = index_;
            address.sll_halen   = aloe::wire::MacAddress::size;
            std::memcpy(address.sll_addr, frame.data(), aloe::wire::MacAddress::size);
            const ssize_t sent = sendto(
                fd_, frame.data(), frame.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            return sent == static_cast<ssize_t>(frame.size());
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

}  // namespace aloe::testing
