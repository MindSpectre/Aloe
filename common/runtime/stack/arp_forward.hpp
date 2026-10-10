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

    /// What one forwarding pass could not deliver: nodes never allocated, and nodes a finished sibling refused.
    struct ForwardOutcome {
        std::size_t dropped  = 0;  ///< Allocation failed; nothing was posted.
        std::size_t rejected = 0;  ///< `post_control` refused; the node was freed here.
    };

    /// One node per sibling. Counts the nodes that could not be allocated and the rejected posts, freed here.
    [[nodiscard]] inline ForwardOutcome forward_resolution(const std::span<ShardContext* const> siblings,
                                                           const wire::Ipv4Address address,
                                                           const wire::MacAddress mac) noexcept {
        ForwardOutcome outcome{};
        for (ShardContext* sibling : siblings) {
            auto* work = new (std::nothrow) ArpForwardWork{*sibling, address, mac};
            if (work == nullptr) {
                ++outcome.dropped;
                continue;
            }
            if (!sibling->post_control(*work)) {
                delete work;  // that shard has finished: nothing reads its inbox
                ++outcome.rejected;
            }
        }
        return outcome;
    }

}  // namespace aloe::runtime::detail
