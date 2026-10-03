#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <device.hpp>
#include <scheduler.hpp>
#include <shard_context.hpp>
#include <shard_queue.hpp>

namespace aloe::runtime {

    /// What a shard does when a step finds nothing to do.
    enum class IdlePolicy : std::uint8_t {
        Spin,   ///< Poll again at once: the production default on an isolated core.
        Yield,  ///< After `yield_after` empty steps, yield the thread once per empty step: tests and CI.
    };

    struct ShardConfig {
        std::chrono::nanoseconds timer_resolution = std::chrono::milliseconds{1};
        std::size_t receive_burst = 64;  ///< Packets one receive call may bring; ethdev moves at most 64.
        std::size_t transmit_ring = 512;
        IdlePolicy idle           = IdlePolicy::Spin;
        std::uint32_t yield_after = 1000;
    };

    /**
     * @brief The layer above a shard.
     *
     * Constructed by the shard from `ShardContext&`, `ShardQueue<Device>&` and the caller's arguments.
     * `on_receive` is called on the shard thread, inside a tick, with a non-empty burst, and must
     * not throw; the stack moves out the packets it keeps and the shard frees the rest. Timers,
     * tasks and scheduling go through the context.
     */
    template <typename S, typename Device>
    concept IsStack = device::IsDevice<Device> && requires(S& stack, std::span<typename Device::Packet> burst) {
        { stack.on_receive(burst) } -> std::same_as<void>;
    };

    /**
     * @brief One shard: a context, one device queue and the stack, with the loop.
     *
     * One tick, `step(now)`: receive a burst, hand it to the stack, `run_once`, flush the transmit
     * ring. Packet work goes before timers and tasks so a reply leaves in the tick its request
     * arrived. `run()` loops `step(clock::now())` until the context is drained, then flushes once
     * more and drops what the device still refuses. Tests call `step` with stamps of their choosing.
     */
    template <device::IsDevice Device, typename Stack>
        requires IsStack<Stack, Device>
    class Shard {
    public:
        using Packet    = typename Device::Packet;
        using Clock     = ShardContext::Clock;
        using TimePoint = ShardContext::TimePoint;

        template <typename... Args>
            requires std::constructible_from<Stack, ShardContext&, ShardQueue<Device>&, Args...>
        Shard(
            const ShardConfig& config, Device& owner, const std::uint16_t index, const TimePoint start, Args&&... args)
            : config_{
                  config
        },
              context_{{.index = index, .timer_resolution = config.timer_resolution}, start},
              queue_{owner, index, config.transmit_ring, context_.counters()},
              stack_{context_, queue_, std::forward<Args>(args)...}, burst_(config.receive_burst) {
        }

        Shard(const Shard&)            = delete;
        Shard& operator=(const Shard&) = delete;
        Shard(Shard&&)                 = delete;
        Shard& operator=(Shard&&)      = delete;
        ~Shard()                       = default;

        /// One tick. Returns whether anything was received, run, fired or sent.
        bool step(const TimePoint now) noexcept {
            const ShardContext::Current current{context_};
            bool busy                  = false;
            const std::size_t received = queue_.receive(burst_);
            if (received > 0) {
                context_.counters().frames_received += received;
                const std::span<Packet> frames{burst_.data(), received};
                stack_.on_receive(frames);
                for (Packet& packet : frames) {
                    if (!packet.empty()) {
                        packet = Packet{};
                    }
                }
                busy = true;
            }
            busy = context_.run_once(now) || busy;
            busy = queue_.flush() > 0 || busy;
            ++context_.counters().ticks;
            if (!busy) {
                ++context_.counters().idle_ticks;
            }
            return busy;
        }

        /// Ticks until the context is drained. Returns with the transmit ring empty.
        void run() noexcept {
            std::uint32_t idle = 0;
            while (!context_.drained()) {
                if (step(Clock::now())) {
                    idle = 0;
                } else if (config_.idle == IdlePolicy::Yield && ++idle >= config_.yield_after) {
                    std::this_thread::yield();
                }
            }
            std::ignore = queue_.flush();
            queue_.discard();
        }

        [[nodiscard]] ShardContext& context() noexcept {
            return context_;
        }

        [[nodiscard]] const ShardContext& context() const noexcept {
            return context_;
        }

        [[nodiscard]] Scheduler scheduler() noexcept {
            return Scheduler{context_};
        }

        [[nodiscard]] ShardQueue<Device>& queue() noexcept {
            return queue_;
        }

        [[nodiscard]] Stack& stack() noexcept {
            return stack_;
        }

        [[nodiscard]] const ShardConfig& config() const noexcept {
            return config_;
        }

    private:
        ShardConfig config_;
        ShardContext context_;
        ShardQueue<Device> queue_;
        Stack stack_;  ///< Declared after the two it holds, so it is destroyed first.
        std::vector<Packet> burst_;
    };

}  // namespace aloe::runtime
