// The same echo as tasks over the runtime and the ready-made TCP stack, on every queue of the port: one shard
// per queue, one task per connection. Compare with examples/tcp_echo for the loop it wraps.
//
//     tcp_echo_tasks <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect <host>:<port>)
//                    [-- <EAL arguments...>]
//
//     sudo tcp_echo_tasks net_tap0 10.78.0.2/24 --listen 7 -- --vdev=net_tap0,iface=aloe0
//         then: ip addr add 10.78.0.1/24 dev aloe0 && ip link set aloe0 up && nc 10.78.0.2 7
//     sudo tcp_echo_tasks net_tap0 10.78.0.2/24 --connect 10.78.0.1:7 -- --vdev=net_tap0,iface=aloe0
//         with a kernel echo server and the interface set up as for tcp_echo: sends a line, prints the echo, closes.
//
// The port opens with one queue, as `ethdev::PortConfig` does by default; a program that opens more gets a shard
// on each, and `runtime::TcpStack` then lets the IP brick learn unsolicited ARP replies so queue 0 can forward
// what its siblings asked for. The client runs on shard 0, whose queue a card with steering off delivers to.

#include <aloe/ethdev>
#include <aloe/net>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <pthread.h>

namespace {

    using Port    = aloe::ethdev::Port;
    using Stack   = aloe::runtime::TcpStack<Port>;
    using Runtime = aloe::runtime::Runtime<Port, Stack>;

    struct Arguments {
        std::string port;
        aloe::net::Ipv4Config ip;
        std::optional<std::uint16_t> listen;
        std::optional<aloe::tcp::Endpoint> connect;
        std::vector<std::string> eal;
    };

    // `parse_port` and `parse` are tcp_echo's, with this program's name in the EAL arguments: each example reads
    // on its own.

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
        arguments.eal.emplace_back("tcp_echo_tasks");
        if (next < argv.size()) {
            if (std::string_view{argv[next]} != "--") {
                return std::nullopt;
            }
            for (char* argument : argv.subspan(next + 1)) {
                arguments.eal.emplace_back(argument);
            }
        }
        if (arguments.eal.size() == 1) {
            arguments.eal = {"tcp_echo_tasks", "--in-memory", "--no-telemetry"};
        }
        return arguments;
    }

    /// One connection: echo until the peer closes, then close. `send` is the copying convenience, and `chunk` must
    /// outlive it, so the consume comes after; prepare, copy, consume, commit would save the window update a flush
    /// sends per echoed segment.
    aloe::runtime::task<void> echo(Stack::StreamType stream) {
        for (;;) {
            const auto readable = co_await stream.readable(1);
            if (!readable) {
                break;  // PeerClosed with nothing left, or Reset, or TimedOut
            }
            while (!stream.unread().empty()) {
                const auto chunk = stream.unread().front();
                if (!co_await stream.send(chunk)) {
                    co_return;
                }
                stream.consume(chunk.size());
            }
        }
        std::ignore = co_await stream.close();
    }

    /// Accepts on one shard's listener and spawns an echo per connection, until the shard stops.
    aloe::runtime::task<void>
    serve(Stack::StreamsType* streams, const aloe::runtime::Scheduler scheduler, const std::uint16_t port) {
        for (;;) {
            auto stream = co_await streams->accept(port);
            if (!stream) {
                break;
            }
            scheduler.spawn(echo(std::move(*stream)));
        }
    }

    constexpr std::string_view hello = "hello from aloe\n";

    aloe::runtime::task<void>
    client(Stack::StreamsType* streams, const aloe::tcp::Endpoint peer, std::atomic<bool>* done) {
        const std::span<const std::byte> bytes = std::as_bytes(std::span{hello});
        auto stream                            = co_await streams->connect(peer);
        if (!stream) {
            std::println(stderr, "connect failed: stream error {}", static_cast<int>(stream.error()));
        } else if (co_await stream->send(bytes)) {
            std::size_t got = 0;
            while (got < bytes.size()) {
                const auto readable = co_await stream->readable(1);
                if (!readable) {
                    break;
                }
                for (const std::span<const std::byte> chunk : stream->unread()) {
                    std::fwrite(chunk.data(), 1, chunk.size(), stdout);
                    got += chunk.size();
                }
                stream->consume(stream->unread().size());
            }
            std::ignore = co_await stream->close();
        }
        done->store(true);
    }

    /// Blocks SIGINT and SIGTERM on this thread, before any other exists, so every shard inherits the mask and
    /// only `sigwait` sees them.
    [[nodiscard]] sigset_t block_termination_signals() {
        sigset_t signals{};
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
        std::println(stderr,
                     "usage: tcp_echo_tasks <port> <address>/<prefix> [<gateway>] (--listen <port> | --connect "
                     "<host>:<port>) [-- <EAL arguments...>]");
        return EXIT_FAILURE;
    }
    const sigset_t signals = block_termination_signals();
    aloe::ethdev::Eal eal{arguments->eal};
    Port port{{.name = arguments->port}};
    Runtime runtime{
        {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
        port,
        arguments->ip,
        aloe::tcp::TcpConfig{}
    };
    runtime.start();
    if (arguments->listen) {
        for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
            runtime.spawn(index,
                          serve(&runtime.shard(index).stack().streams(), runtime.scheduler(index), *arguments->listen));
        }
        std::println("{} queues echoing on port {}; interrupt to stop", runtime.shard_count(), *arguments->listen);
        int signal = 0;
        sigwait(&signals, &signal);
    } else {
        std::atomic<bool> done{false};
        runtime.spawn(0, client(&runtime.shard(0).stack().streams(), *arguments->connect, &done));
        constexpr timespec poll_interval{.tv_sec = 0, .tv_nsec = 1'000'000};
        while (!done.load() && sigtimedwait(&signals, nullptr, &poll_interval) < 0) {
            // an unanswered ARP keeps the SYN waiting without a time-out: an interrupt ends the wait
        }
    }
    runtime.stop();
    runtime.join();
    std::fflush(stdout);
    for (std::uint16_t index = 0; index < runtime.shard_count(); ++index) {
        Stack& stack                           = runtime.shard(index).stack();
        const aloe::tcp::TcpCounters& counters = stack.tcp().counters();
        std::println("shard {}: {} accepted, {} opened, {} closed, {} reset, {} timed out, {} data segments, "
                     "{} pure ACKs, {} dropped without a connection; ARP forwards: {} dropped, {} rejected",
                     index,
                     counters.connections_accepted,
                     counters.connections_opened,
                     counters.connections_closed,
                     counters.connections_reset,
                     counters.connections_timed_out,
                     counters.data_segments_sent,
                     counters.pure_acks_sent,
                     counters.dropped_no_connection,
                     stack.forwards_dropped(),
                     stack.forwards_rejected());
    }
    return EXIT_SUCCESS;
}
