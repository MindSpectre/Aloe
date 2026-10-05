// Answers ARP and ping on one queue of a DPDK port with a hand-written loop over the bricks, until interrupted.
//
//     ping_responder <port> <address>/<prefix> [<gateway>] [-- <EAL arguments...>]
//
//     sudo ping_responder net_tap0 10.77.0.2/24 -- --vdev=net_tap0,iface=aloe0
//         then, in another shell: ip addr add 10.77.0.1/24 dev aloe0 && ip link set aloe0 up && ping 10.77.0.2
//     ping_responder 0000:03:00.0 192.168.1.50/24 192.168.1.1 -- -l 0 --file-prefix ping    # a real card
//
// No runtime and no thread but this one: the loop is the one docs/architecture/net.md leads with. With a
// gateway given, its MAC is asked for at start, so the first off-subnet send would find it.

#include <aloe/device>
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/wire>
#include <atomic>
#include <charconv>
#include <chrono>
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
#include <tuple>
#include <vector>

namespace {

    std::atomic<bool> interrupted{false};
    static_assert(std::atomic<bool>::is_always_lock_free, "a signal handler may only touch a lock-free atomic");

    void on_signal(int /*signal*/) {
        interrupted.store(true);
    }

    struct Arguments {
        std::string port;
        aloe::net::Ipv4Config config;
        std::vector<std::string> eal;
    };

    [[nodiscard]] std::optional<Arguments> parse(const std::span<char*> argv) {
        if (argv.size() < 3) {
            return std::nullopt;
        }
        Arguments arguments;
        arguments.port = argv[1];

        const std::string_view cidr{argv[2]};
        const std::size_t slash = cidr.find('/');
        if (slash == std::string_view::npos) {
            return std::nullopt;
        }
        const auto address = aloe::wire::Ipv4Address::parse(cidr.substr(0, slash));
        unsigned prefix    = 0;
        const auto length  = cidr.substr(slash + 1);
        if (!address || std::from_chars(length.data(), length.data() + length.size(), prefix).ec != std::errc{} ||
            prefix > 32) {
            return std::nullopt;
        }
        arguments.config.address = *address;
        arguments.config.prefix  = static_cast<std::uint8_t>(prefix);

        std::size_t next = 3;
        if (next < argv.size() && std::string_view{argv[next]} != "--") {
            const auto gateway = aloe::wire::Ipv4Address::parse(argv[next]);
            if (!gateway) {
                return std::nullopt;
            }
            arguments.config.gateway = *gateway;
            ++next;
        }
        arguments.eal.emplace_back("ping_responder");
        if (next < argv.size()) {
            if (std::string_view{argv[next]} != "--") {
                return std::nullopt;
            }
            for (char* argument : argv.subspan(next + 1)) {
                arguments.eal.emplace_back(argument);
            }
        }
        if (arguments.eal.size() == 1) {
            arguments.eal = {"ping_responder", "--in-memory", "--no-telemetry"};
        }
        return arguments;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    if (!arguments) {
        std::println(stderr, "usage: ping_responder <port> <address>/<prefix> [<gateway>] [-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    aloe::ethdev::Eal eal{arguments->eal};
    aloe::ethdev::Port port{
        {.name = arguments->port, .queues = 1}
    };
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<aloe::ethdev::Port> queue{port, 0, 64, counters};
    aloe::net::Ipv4<aloe::ethdev::Port> ip{queue, arguments->config};
    std::vector<aloe::ethdev::Packet> burst(64);
    std::println("{} ({}) MAC {}: answering ARP and ping for {}/{}; interrupt to stop",
                 arguments->port,
                 port.driver_name(),
                 port.mac(),
                 ip.address(),
                 ip.prefix());
    if (ip.gateway()) {
        std::ignore = ip.resolve(*ip.gateway(), std::chrono::steady_clock::now());  // the request leaves now
        std::ignore = queue.flush();
    }

    while (!interrupted.load()) {
        const auto now             = std::chrono::steady_clock::now();
        const std::size_t received = queue.receive(burst);
        ip.process(std::span<aloe::ethdev::Packet>{burst}.first(received), now);
        for (const aloe::net::ArpResolution& resolution : ip.resolved()) {  // a few lines a minute, never per packet
            std::println("resolved {} at {}", resolution.address, resolution.mac);
        }
        std::ignore = queue.flush();
    }
    queue.discard();

    const aloe::net::Ipv4Counters& c = ip.counters();
    std::println("{} frames: {} ARP requests answered, {} echoes answered, {} resolutions; dropped: {} not for us, "
                 "{} other ethertypes, {} bad checksums, {} other",
                 c.frames,
                 c.arp_replies_sent,
                 c.echo_replies_sent,
                 c.resolutions,
                 c.dropped_not_for_us,
                 c.dropped_ethertype,
                 c.dropped_bad_checksum,
                 c.dropped_short + c.dropped_arp_malformed + c.dropped_arp_conflict + c.dropped_arp_unsolicited +
                     c.dropped_bad_header + c.dropped_fragment + c.dropped_protocol + c.dropped_icmp);
    return EXIT_SUCCESS;
}
