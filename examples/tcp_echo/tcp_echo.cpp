// A TCP echo server or client on one queue of a DPDK port, as a hand-written loop over the bricks: no runtime,
// no coroutine, no sender on the data path. The loop docs/architecture/tcp.md leads with.
//
//     tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>)
//              [-- <EAL arguments...>]
//
//     sudo tcp_echo net_tap0 10.78.0.2/24 --listen 7 -- --vdev=net_tap0,iface=aloe0
//         then: ip addr add 10.78.0.1/24 dev aloe0 && ip link set aloe0 up && nc 10.78.0.2 7
//     sudo tcp_echo net_tap0 10.78.0.2/24 --connect 10.78.0.1:7 -- --vdev=net_tap0,iface=aloe0
//         with an echo server already listening on the kernel side, `socat TCP-LISTEN:7,reuseaddr,fork EXEC:cat`,
//         then address aloe0 as above: the SYN waits for ARP until the interface answers. The client sends a
//         line, prints the echo, closes.
//
// The echo's copy from the received packet into the prepared one is the application's operation: a program
// producing new messages encodes them straight into `prepare` and never copies. A multi-queue program over the
// bricks sets `net::Ipv4Config::accept_unsolicited_replies`, since every queue shares the address; this one runs
// on one queue and leaves it off.

#include <algorithm>
#include <aloe/core>
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
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

    using Port = aloe::ethdev::Port;
    using Ipv4 = aloe::net::Ipv4<Port>;
    using Tcp  = aloe::tcp::Stack<Ipv4>;

    std::atomic<bool> interrupted{false};
    static_assert(std::atomic<bool>::is_always_lock_free, "a signal handler may only touch a lock-free atomic");

    void on_signal(int /*signal*/) {
        interrupted.store(true);
    }

    struct Arguments {
        std::string port;
        aloe::net::Ipv4Config ip;
        std::optional<std::uint16_t> listen;
        std::optional<aloe::tcp::Endpoint> connect;
        std::vector<std::string> eal;
    };

    [[nodiscard]] std::optional<std::uint16_t> parse_port(const std::string_view text) {
        unsigned value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || value == 0 ||
            value > 65535) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(value);
    }

    [[nodiscard]] std::optional<Arguments> parse(const std::span<char*> argv) {
        if (argv.size() < 4) {
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
        arguments.ip.address = *address;
        arguments.ip.prefix  = static_cast<std::uint8_t>(prefix);
        std::size_t next     = 3;
        if (!std::string_view{argv[next]}.starts_with("--")) {
            const auto gateway = aloe::wire::Ipv4Address::parse(argv[next]);
            if (!gateway) {
                return std::nullopt;
            }
            arguments.ip.gateway = *gateway;
            ++next;
        }
        if (next + 1 >= argv.size()) {
            return std::nullopt;
        }
        const std::string_view role{argv[next]};
        const std::string_view target{argv[next + 1]};
        if (role == "--listen") {
            arguments.listen = parse_port(target);
            if (!arguments.listen) {
                return std::nullopt;
            }
        } else if (role == "--connect") {
            const std::size_t colon = target.rfind(':');
            if (colon == std::string_view::npos) {
                return std::nullopt;
            }
            const auto host = aloe::wire::Ipv4Address::parse(target.substr(0, colon));
            const auto port = parse_port(target.substr(colon + 1));
            if (!host || !port) {
                return std::nullopt;
            }
            arguments.connect = aloe::tcp::Endpoint{.address = *host, .port = *port};
        } else {
            return std::nullopt;
        }
        next += 2;
        arguments.eal.emplace_back("tcp_echo");
        if (next < argv.size()) {
            if (std::string_view{argv[next]} != "--") {
                return std::nullopt;
            }
            for (char* argument : argv.subspan(next + 1)) {
                arguments.eal.emplace_back(argument);
            }
        }
        if (arguments.eal.size() == 1) {
            arguments.eal = {"tcp_echo", "--in-memory", "--no-telemetry"};
        }
        return arguments;
    }

    /// The echo: what was read goes back out, in segments of what the connection can take; the rest waits for
    /// Writable. It consumes after the commit, so a refused commit loses nothing; the price is a window update
    /// at the flush for every segment echoed.
    void echo(Tcp::ConnectionType& c) {
        while (!c.unread().empty()) {
            const std::span<const std::byte> chunk = c.unread().front();
            const auto out                         = c.prepare(chunk.size());
            if (!out) {
                return;
            }
            std::ranges::copy(chunk.first(out->size()), out->begin());
            if (!c.commit(out->size())) {
                return;
            }
            c.consume(out->size());
        }
    }

    constexpr std::string_view hello = "hello from aloe\n";

    [[nodiscard]] std::string_view to_string(const aloe::tcp::ConnectError error) {
        switch (error) {
            case aloe::tcp::ConnectError::TableFull:
                return "no free connection slot";
            case aloe::tcp::ConnectError::NoPort:
                return "no free local port";
            case aloe::tcp::ConnectError::Unplaceable:
                return "no local port lands on this queue";
            case aloe::tcp::ConnectError::NoRoute:
                return "no route: off the subnet and no gateway";
        }
        return "unknown";
    }

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    if (!arguments) {
        std::println(
            stderr,
            "usage: tcp_echo <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>) "
            "[-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    aloe::ethdev::Eal eal{arguments->eal};
    Port port{
        {.name = arguments->port, .queues = 1}
    };
    aloe::loop::ShardCounters counters;
    aloe::loop::ShardQueue<Port> queue{port, 0, 512, counters};
    Ipv4 ip{queue, arguments->ip};
    aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, aloe::core::Clock::now()};
    Tcp tcp{ip, wheel, {}};
    std::vector<Port::Packet> burst(64);
    const std::span<const std::byte> greeting = std::as_bytes(std::span{hello});
    std::size_t offered                       = 0;  // the client's bytes of the greeting committed so far
    std::size_t echoed                        = 0;  // the client's bytes of the echo printed so far

    if (arguments->listen) {
        if (!tcp.listen(*arguments->listen)) {
            std::println(stderr, "cannot listen on {}", *arguments->listen);
            return EXIT_FAILURE;
        }
        std::println("{} ({}) {}: echoing on port {}; interrupt to stop",
                     arguments->port,
                     port.driver_name(),
                     ip.address(),
                     *arguments->listen);
    } else {
        const auto session = tcp.connect(*arguments->connect, aloe::core::Clock::now());
        if (!session) {
            std::println(stderr, "connect failed: {}", to_string(session.error()));
            return EXIT_FAILURE;
        }
        std::println("{} ({}) {}: connecting to {}:{}",
                     arguments->port,
                     port.driver_name(),
                     ip.address(),
                     arguments->connect->address,
                     arguments->connect->port);
    }

    while (!interrupted.load()) {
        const auto now             = aloe::core::Clock::now();
        const std::size_t received = queue.receive(burst);
        ip.process(std::span<Port::Packet>{burst}.first(received), now);
        tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
        std::ignore = wheel.advance(now);  // timer events join the receive events before the drain

        while (const auto event = tcp.poll_event()) {
            Tcp::ConnectionType& c            = *event->connection;
            const aloe::stream::Events events = event->events;
            if (arguments->connect && (events.connected() || events.writable()) && offered < greeting.size()) {
                offered += c.send(greeting.subspan(offered));  // a short send resumes on Writable
            }
            if (events.readable() || events.writable()) {
                if (arguments->listen) {
                    echo(c);
                } else {
                    for (const std::span<const std::byte> chunk : c.unread()) {
                        std::fwrite(chunk.data(), 1, chunk.size(), stdout);
                        echoed += chunk.size();
                    }
                    c.consume(c.unread().size());
                    if (echoed >= greeting.size() || c.peer_closed()) {
                        c.close();  // a no-op once closing
                    }
                }
            }
            if (arguments->listen && c.peer_closed() && c.unread().empty() &&
                c.state() == aloe::tcp::State::CloseWait) {
                c.close();  // everything the peer sent went back: our FIN follows it
            }
            if (events.closed() || events.reset() || events.timed_out()) {
                c.release();
                if (arguments->connect) {
                    interrupted.store(true);  // the client's one session is over
                }
            }
        }

        tcp.flush(now);
        std::ignore = queue.flush();
    }
    std::fflush(stdout);
    queue.discard();

    const aloe::tcp::TcpCounters& c = tcp.counters();
    std::println("{} segments in, {} data out, {} pure ACKs, {} control, {} resets; {} accepted, {} opened, {} closed, "
                 "{} reset, {} timed out; dropped: {} no connection, {} checksum, {} window, {} resources",
                 c.segments_received,
                 c.data_segments_sent,
                 c.pure_acks_sent,
                 c.control_segments_sent,
                 c.resets_sent,
                 c.connections_accepted,
                 c.connections_opened,
                 c.connections_closed,
                 c.connections_reset,
                 c.connections_timed_out,
                 c.dropped_no_connection,
                 c.dropped_bad_checksum,
                 c.dropped_out_of_window + c.dropped_out_of_order + c.dropped_duplicate,
                 c.dropped_no_slot + c.dropped_no_node + c.dropped_table_full + c.allocation_failures);
    return EXIT_SUCCESS;
}
