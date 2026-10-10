#pragma once

#include <cstddef>
#include <new>
#include <span>

#include <ipv4_address.hpp>
#include <mac_address.hpp>
#include <shard_context.hpp>
#include <work.hpp>

namespace aloe::runtime::detail {

    /**
     * @brief A resolution on its way to a sibling shard.
     *
     * Allocated on the cold path by the shard that learned it, posted through `post_control`, and
     * freed by the target after delivery; a rejected post is freed by the sender. Carries no device
     * and no packet: an address and a MAC.
     */
    struct ArpForwardWork : loop::Work {
        ArpForwardWork(ShardContext& to, const wire::Ipv4Address resolved, const wire::MacAddress at) noexcept
            : loop::Work{&ArpForwardWork::deliver},
              target{&to},
              address{resolved},
              mac{at} {
        }

        /// Runs once on the target's thread: hands the resolution to its sink, then frees the node.
        static void deliver(loop::Work& work) noexcept {
            auto* self = static_cast<ArpForwardWork*>(&work);
            self->target->deliver_arp(self->address, self->mac);
            delete self;
        }

        ShardContext* target = nullptr;
        wire::Ipv4Address address{};
        wire::MacAddress mac{};
    };

    /// One node per sibling. Returns how many could not be allocated; a rejected post frees its node here.
    [[nodiscard]] inline std::size_t forward_resolution(const std::span<ShardContext* const> siblings,
                                                        const wire::Ipv4Address address,
                                                        const wire::MacAddress mac) noexcept {
        std::size_t dropped = 0;
        for (ShardContext* sibling : siblings) {
            auto* work = new (std::nothrow) ArpForwardWork{*sibling, address, mac};
            if (work == nullptr) {
                ++dropped;
                continue;
            }
            if (!sibling->post_control(*work)) {
                delete work;  // that shard has finished: nothing reads its inbox
            }
        }
        return dropped;
    }

}  // namespace aloe::runtime::detail
