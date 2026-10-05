// Runs the Ethernet echo on N shards of one DPDK port until interrupted, then prints the counters.
//
//     ethernet_echo <port> <queues> [-- <EAL arguments...>]
//
//     ethernet_echo net_tap0 1 -- --vdev=net_tap0,iface=aloe0      # a tap, as root
//     ethernet_echo 0000:03:00.0 4 -- -l 0 --file-prefix echo      # a real card
//
// Frames addressed to the port's MAC come back with the addresses swapped.

#include <algorithm>
#include <aloe/device>
#include <aloe/ethdev>
#include <aloe/log>
#include <aloe/loop>
#include <aloe/runtime>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <pthread.h>

namespace {

    /// The whole stack: answer frames addressed to me, swapping the addresses.
    class EchoStack {
    public:
        using Packet = aloe::ethdev::Packet;

        EchoStack(aloe::runtime::ShardContext& /*context*/, aloe::loop::ShardQueue<aloe::ethdev::Port>& queue) noexcept
            : queue_{&queue} {
        }

        void on_receive(std::span<Packet> burst) noexcept {
            for (Packet& packet : burst) {
                const std::span<std::byte> data = packet.data();
                if (data.size() < aloe::device::ethernet_header_size || !addressed_to_me(data)) {
                    continue;  // the shard frees what we leave
                }
                std::swap_ranges(data.begin(), data.begin() + 6, data.begin() + 6);
                if (!queue_->transmit(std::move(packet))) {
                    ++refused_;
                }
            }
        }

        [[nodiscard]] std::uint64_t refused() const noexcept {
            return refused_;
        }

    private:
        [[nodiscard]] bool addressed_to_me(const std::span<const std::byte> frame) const noexcept {
            aloe::device::MacAddress::Bytes destination{};
            std::ranges::copy(frame.first(aloe::device::MacAddress::size), destination.begin());
            return aloe::device::MacAddress{destination} == queue_->mac();
        }

        aloe::loop::ShardQueue<aloe::ethdev::Port>* queue_;
        std::uint64_t refused_ = 0;
    };

    struct Arguments {
        std::string port;
        std::uint16_t queues = 1;
        std::vector<std::string> eal;
    };

    [[nodiscard]] std::optional<Arguments> parse(const std::span<char*> argv) {
        if (argv.size() < 3) {
            return std::nullopt;
        }
        Arguments arguments;
        arguments.port = argv[1];
        const std::string_view queues{argv[2]};
        if (std::from_chars(queues.data(), queues.data() + queues.size(), arguments.queues).ec != std::errc{} ||
            arguments.queues == 0) {
            return std::nullopt;
        }
        arguments.eal.emplace_back("ethernet_echo");
        bool after_separator = false;
        for (char* argument : argv.subspan(3)) {
            if (!after_separator) {
                if (std::string_view{argument} != "--") {
                    return std::nullopt;
                }
                after_separator = true;
                continue;
            }
            arguments.eal.emplace_back(argument);
        }
        if (arguments.eal.size() == 1) {
            arguments.eal = {"ethernet_echo", "--in-memory", "--no-telemetry"};
        }
        return arguments;
    }

    /// Blocks SIGINT and SIGTERM for the calling thread and every thread it starts afterwards.
    [[nodiscard]] sigset_t block_termination_signals() {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        return signals;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    if (!arguments) {
        std::println(stderr, "usage: ethernet_echo <port> <queues> [-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    const sigset_t signals = block_termination_signals();

    aloe::log::Logging logging{{.level = aloe::log::LogLevel::Info}};
    aloe::ethdev::Eal eal{arguments->eal};
    aloe::ethdev::Port port{
        {.name = arguments->port, .queues = arguments->queues}
    };
    std::println("{} ({}) with {} queues, MAC {}",
                 arguments->port,
                 port.driver_name(),
                 port.queue_count(),
                 port.mac().to_string());

    aloe::runtime::Runtime<aloe::ethdev::Port, EchoStack> runtime{
        {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
        port
    };
    runtime.start();
    std::println("echoing on {} shards; interrupt to stop", runtime.shard_count());

    int signal = 0;
    sigwait(&signals, &signal);
    runtime.stop();
    runtime.join();

    for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
        const auto& counters = runtime.counters(index);
        std::println("shard {}: {} ticks, {} frames in, {} out, {} refused, {} idle ticks",
                     index,
                     counters.ticks,
                     counters.frames_received,
                     counters.frames_transmitted,
                     counters.transmit_refused + runtime.shard(index).stack().refused(),
                     counters.idle_ticks);
    }
    return EXIT_SUCCESS;
}
