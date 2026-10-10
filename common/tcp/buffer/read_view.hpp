#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

#include <packet.hpp>
#include <tcp_node_pool.hpp>

namespace aloe::tcp {

    /**
     * @brief The unread bytes of a connection: one span per held packet, over the pool's storage.
     *
     * A forward range whose value type is `std::span<const std::byte>`; `size()` counts bytes. The
     * view is a pair of indices and a count, copied freely, and is invalidated by `consume` and by
     * the next `process`. The pool pointer is mutable because the packet concept offers `data()`
     * only on a non-const packet; the bytes handed out are const.
     */
    template <device::IsPacket Packet>
    class ReadView {
    public:
        using Pool = detail::TcpNodePool<Packet>;

        class Iterator {
        public:
            using iterator_concept  = std::forward_iterator_tag;
            using iterator_category = std::input_iterator_tag;  // the reference is a prvalue span
            using value_type        = std::span<const std::byte>;
            using difference_type   = std::ptrdiff_t;

            Iterator() noexcept = default;

            Iterator(Pool* pool, const std::uint32_t index) noexcept
                : pool_{pool},
                  index_{index} {
            }

            [[nodiscard]] std::span<const std::byte> operator*() const noexcept {
                typename Pool::Node& node = pool_->node(index_);
                return std::span<const std::byte>{node.packet.data()}.subspan(node.offset, node.length);
            }

            Iterator& operator++() noexcept {
                index_ = pool_->node(index_).next;
                return *this;
            }

            Iterator operator++(int) noexcept {
                const Iterator before = *this;
                ++*this;
                return before;
            }

            friend bool operator==(const Iterator&, const Iterator&) noexcept = default;

        private:
            Pool* pool_          = nullptr;
            std::uint32_t index_ = detail::no_node;
        };

        ReadView() noexcept = default;

        ReadView(Pool& pool, const std::uint32_t head, const std::size_t bytes) noexcept
            : pool_{&pool},
              head_{head},
              bytes_{bytes} {
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return bytes_ == 0;
        }

        [[nodiscard]] std::span<const std::byte> front() const noexcept {
            return *begin();
        }

        [[nodiscard]] Iterator begin() const noexcept {
            return Iterator{pool_, bytes_ == 0 ? detail::no_node : head_};
        }

        [[nodiscard]] Iterator end() const noexcept {
            return Iterator{pool_, detail::no_node};
        }

    private:
        Pool* pool_         = nullptr;
        std::uint32_t head_ = detail::no_node;
        std::size_t bytes_  = 0;
    };

}  // namespace aloe::tcp
