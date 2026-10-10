#pragma once

#include <aloe/core>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

#include <device.hpp>
#include <ipv4.hpp>
#include <ipv4_address.hpp>
#include <shard_queue.hpp>

namespace aloe::tcp {

    template <typename Ip>
    class Connection;

    template <typename Ip>
    struct ConnectionEvent;

    template <typename Ip>
    class Stack;

    /// What the brick asks of the IP layer below it: `net::Ipv4<Device>`, or anything with these members.
    template <typename Ip>
    concept IsIp =
        device::IsDevice<typename Ip::Device> && std::same_as<typename Ip::Packet, typename Ip::Device::Packet> &&
        requires(Ip& ip,
                 const Ip& const_ip,
                 typename Ip::Packet packet,
                 const net::SendRequest& request,
                 core::TimePoint now,
                 wire::Ipv4Address address) {
            { ip.allocate() } -> std::same_as<std::optional<typename Ip::Packet>>;
            { ip.send(std::move(packet), request, now) } -> std::same_as<std::expected<void, net::SendError>>;
            { const_ip.address() } -> std::same_as<wire::Ipv4Address>;
            { const_ip.max_l4_size() } -> std::same_as<std::uint16_t>;
            { const_ip.next_hop(address) } -> std::same_as<std::optional<wire::Ipv4Address>>;
            { ip.queue() } -> std::same_as<loop::ShardQueue<typename Ip::Device>&>;
        };

    namespace detail {

        /// The links of one intrusive list membership; a connection has one per list it can be on.
        template <typename C>
        struct Link {
            C* prev     = nullptr;
            C* next     = nullptr;
            bool linked = false;
        };

        /**
         * @brief An intrusive doubly linked list of connections through one of their Link members.
         *
         * Constant-time push, remove and pop; a connection is on a list at most once, which `push_back`
         * asserts. No allocation; the links live in the connections.
         */
        template <typename C, Link<C> C::* Member>
        class ConnectionList {
        public:
            void push_back(C& item) noexcept {
                Link<C>& link = item.*Member;
                assert(!link.linked && "a connection is on a list at most once");
                link.prev   = tail_;
                link.next   = nullptr;
                link.linked = true;
                if (tail_ != nullptr) {
                    (tail_->*Member).next = &item;
                } else {
                    head_ = &item;
                }
                tail_ = &item;
                ++size_;
            }

            /// A no-op for a connection that is not on the list.
            void remove(C& item) noexcept {
                Link<C>& link = item.*Member;
                if (!link.linked) {
                    return;
                }
                if (link.prev != nullptr) {
                    (link.prev->*Member).next = link.next;
                } else {
                    head_ = link.next;
                }
                if (link.next != nullptr) {
                    (link.next->*Member).prev = link.prev;
                } else {
                    tail_ = link.prev;
                }
                link = Link<C>{};
                --size_;
            }

            [[nodiscard]] C* pop_front() noexcept {
                C* item = head_;
                if (item != nullptr) {
                    remove(*item);
                }
                return item;
            }

            [[nodiscard]] C* front() const noexcept {
                return head_;
            }

            [[nodiscard]] static C* next_of(C& item) noexcept {
                return (item.*Member).next;
            }

            [[nodiscard]] bool contains(const C& item) const noexcept {
                return (item.*Member).linked;
            }

            [[nodiscard]] bool empty() const noexcept {
                return head_ == nullptr;
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return size_;
            }

        private:
            C* head_          = nullptr;
            C* tail_          = nullptr;
            std::size_t size_ = 0;
        };

    }  // namespace detail

}  // namespace aloe::tcp
