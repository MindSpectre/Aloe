#include <algorithm>
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <logging_environment.hpp>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Needs CAP_NET_ADMIN: DPDK's tap driver creates a kernel interface, the test addresses it, and kernel sockets
// talk TCP to the brick and to the runtime, in both directions. Run it as root, outside the presets:
//
//     sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap
//
// The kernel's SYN carries SACK-permitted, timestamps and window scale, which the option parser skips; the tap
// offers no offloads, so every checksum is software on our side and verified by the kernel.
namespace {

    using namespace std::chrono_literals;
    using Port    = aloe::ethdev::Port;
    using Ipv4    = aloe::net::Ipv4<Port>;
    using Tcp     = aloe::tcp::Stack<Ipv4>;
    using Stack   = aloe::runtime::TcpStack<Port>;
    using Runtime = aloe::runtime::Runtime<Port, Stack>;

    constexpr std::string_view interface = "aloe-tcp";
    constexpr aloe::wire::Ipv4Address kernel_ip{10, 78, 0, 1};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 78, 0, 2};
    constexpr aloe::wire::Ipv4Address netmask{255, 255, 255, 0};
    constexpr std::uint16_t echo_port = 7;
    constexpr auto patience           = 5000ms;

    const auto* const environment =
        ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_tap0,iface=aloe-tcp"});
    const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});

    /// `ip addr add` and `ip link set up`, by ioctl: the kernel end of the tap.
    void configure_interface(const std::string_view name,
                             const aloe::wire::Ipv4Address address,
                             const aloe::wire::Ipv4Address mask) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(fd, 0) << std::strerror(errno);
        ifreq request{};
        std::strncpy(request.ifr_name, std::string{name}.c_str(), IFNAMSIZ - 1);
        auto& in      = *reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
        in.sin_family = AF_INET;
        std::memcpy(&in.sin_addr, address.bytes().data(), aloe::wire::Ipv4Address::size);
        EXPECT_EQ(ioctl(fd, SIOCSIFADDR, &request), 0) << "SIOCSIFADDR: " << std::strerror(errno);
        std::memcpy(&in.sin_addr, mask.bytes().data(), aloe::wire::Ipv4Address::size);
        EXPECT_EQ(ioctl(fd, SIOCSIFNETMASK, &request), 0) << "SIOCSIFNETMASK: " << std::strerror(errno);
        EXPECT_EQ(ioctl(fd, SIOCGIFFLAGS, &request), 0) << "SIOCGIFFLAGS: " << std::strerror(errno);
        request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
        EXPECT_EQ(ioctl(fd, SIOCSIFFLAGS, &request), 0) << "SIOCSIFFLAGS: " << std::strerror(errno);
        close(fd);
    }

    [[nodiscard]] sockaddr_in address_of(const aloe::wire::Ipv4Address address, const std::uint16_t port) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port   = htons(port);
        std::memcpy(&in.sin_addr, address.bytes().data(), aloe::wire::Ipv4Address::size);
        return in;
    }

    /// Writes everything or fails; partial writes and EINTR are the kernel's business, not the test's.
    [[nodiscard]] bool send_all(const int fd, std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
        }
        return true;
    }

    /// Reads exactly `out.size()` bytes within `patience`, or returns false.
    [[nodiscard]] bool recv_all(const int fd, std::span<std::byte> out) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!out.empty()) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left <= 0ms) {
                return false;
            }
            pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
            const int ready = poll(&waiting, 1, static_cast<int>(left.count()));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return false;
            }
            const ssize_t got = ::recv(fd, out.data(), out.size(), 0);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                return false;
            }
            out = out.subspan(static_cast<std::size_t>(got));
        }
        return true;
    }

    /// True when the peer closed: a zero-length read within `patience`.
    [[nodiscard]] bool recv_eof(const int fd) {
        std::array<std::byte, 1> one{};
        pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&waiting, 1, static_cast<int>(patience.count())) <= 0) {
            return false;
        }
        return ::recv(fd, one.data(), one.size(), 0) == 0;
    }

    /// Polls `done` until it holds or `patience` runs out; true when it held.
    template <typename Predicate>
    [[nodiscard]] bool eventually(Predicate done) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// The hand-written echo over the bricks on its own thread: server when `listen`, client to `peer` otherwise.
    class BrickLoop {
    public:
        BrickLoop(Port& port, const bool listen, const std::span<const std::byte> to_send)
            : thread_{
                  [this, &port, listen, to_send](const std::stop_token& stop) { run(port, listen, to_send, stop); }} {
        }

        BrickLoop(const BrickLoop&)            = delete;
        BrickLoop& operator=(const BrickLoop&) = delete;

        ~BrickLoop() {
            stop();
        }

        void stop() {
            if (thread_.joinable()) {
                thread_.request_stop();
                thread_.join();
            }
        }

        [[nodiscard]] const aloe::tcp::TcpCounters& counters() const noexcept {
            return counters_;
        }
        [[nodiscard]] const std::vector<std::byte>& received() const noexcept {
            return received_;
        }
        [[nodiscard]] bool clean_close() const noexcept {
            return clean_close_.load(std::memory_order_acquire);
        }
        std::atomic<bool> connected{false};

    private:
        void run(Port& port, const bool listen, const std::span<const std::byte> to_send, const std::stop_token& stop) {
            aloe::ethdev::register_thread();
            aloe::loop::ShardCounters shard_counters;
            aloe::loop::ShardQueue<Port> queue{port, 0, 64, shard_counters};
            Ipv4 ip{
                queue, {.address = stack_ip, .prefix = 24}
            };
            aloe::loop::TimerWheel wheel{1ms, std::chrono::steady_clock::now()};
            Tcp tcp{ip, wheel, {}};
            std::vector<Port::Packet> burst(64);
            std::size_t offered = 0;
            if (listen) {
                std::ignore = tcp.listen(echo_port);
            } else {
                std::ignore = tcp.connect({.address = kernel_ip, .port = echo_port}, std::chrono::steady_clock::now());
            }
            while (!stop.stop_requested()) {
                const auto now             = std::chrono::steady_clock::now();
                const std::size_t received = queue.receive(burst);
                ip.process(std::span<Port::Packet>{burst}.first(received), now);
                tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
                std::ignore = wheel.advance(now);
                while (const auto event = tcp.poll_event()) {
                    auto& c = *event->connection;
                    if (event->events.connected() || event->events.accepted()) {
                        connected.store(true);
                    }
                    if ((event->events.connected() || event->events.writable()) && !listen) {
                        offered += c.send(to_send.subspan(offered));
                    }
                    if (event->events.readable() || event->events.writable()) {
                        if (listen) {
                            echo(c);
                        } else {
                            for (const std::span<const std::byte> chunk : c.unread()) {
                                received_.insert(received_.end(), chunk.begin(), chunk.end());
                            }
                            c.consume(c.unread().size());
                            if (received_.size() == to_send.size()) {
                                c.close();
                            }
                        }
                    }
                    if (listen && c.peer_closed() && c.unread().empty() && c.state() == aloe::tcp::State::CloseWait) {
                        c.close();
                    }
                    if (event->events.closed()) {
                        clean_close_.store(c.state() == aloe::tcp::State::Closed && !event->events.reset(),
                                           std::memory_order_release);
                        c.release();
                    } else if (event->events.reset() || event->events.timed_out()) {
                        c.release();
                    }
                }
                tcp.flush(now);
                std::ignore = queue.flush();
                if (received == 0) {
                    std::this_thread::yield();
                }
            }
            counters_ = tcp.counters();
            queue.discard();
        }

        static void echo(Tcp::ConnectionType& c) {
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

        aloe::tcp::TcpCounters counters_{};
        std::vector<std::byte> received_;
        std::atomic<bool> clean_close_{false};  ///< Written by the loop thread, polled by the test thread.
        std::jthread thread_;
    };

    /// The echo as tasks over the runtime.
    aloe::runtime::task<void> echo_task(Stack::StreamType stream, std::atomic<int>* closed) {
        for (;;) {
            const auto readable = co_await stream.readable(1);
            if (!readable) {
                break;
            }
            while (!stream.unread().empty()) {
                const auto chunk = stream.unread().front();
                if (!co_await stream.send(chunk)) {
                    co_return;
                }
                stream.consume(chunk.size());
            }
        }
        if (const auto done = co_await stream.close(); done) {
            closed->fetch_add(1, std::memory_order_release);
        }
    }

    /// Counts the connections it accepts and the echoes that closed cleanly. `listening` is set just before the
    /// first `accept` starts: the sender creates the listener synchronously on this shard thread, before the shard
    /// processes another frame, so once the flag is visible a kernel SYN finds the listener.
    aloe::runtime::task<void> serve_task(Stack::StreamsType* streams,
                                         aloe::runtime::Scheduler scheduler,
                                         std::atomic<bool>* listening,
                                         std::atomic<int>* served,
                                         std::atomic<int>* closed) {
        for (;;) {
            listening->store(true, std::memory_order_release);
            auto stream = co_await streams->accept(echo_port);
            if (!stream) {
                break;
            }
            served->fetch_add(1);
            scheduler.spawn(echo_task(std::move(*stream), closed));
        }
    }

    aloe::runtime::task<void> client_task(Stack::StreamsType* streams,
                                          const std::span<const std::byte> to_send,
                                          std::vector<std::byte>* received,
                                          std::atomic<int>* outcome) {
        auto stream = co_await streams->connect({.address = kernel_ip, .port = echo_port});
        if (!stream) {
            outcome->store(-1);
            co_return;
        }
        if (!co_await stream->send(to_send)) {
            outcome->store(-2);
            co_return;
        }
        while (received->size() < to_send.size()) {
            const auto readable = co_await stream->readable(1);
            if (!readable) {
                outcome->store(-3);
                co_return;
            }
            for (const std::span<const std::byte> chunk : stream->unread()) {
                received->insert(received->end(), chunk.begin(), chunk.end());
            }
            stream->consume(stream->unread().size());
        }
        const auto closed = co_await stream->close();
        outcome->store(closed ? 1 : -4);
    }

    [[nodiscard]] std::vector<std::byte> message(const std::size_t size) {
        std::vector<std::byte> out(size);
        for (std::size_t index = 0; index < size; ++index) {
            out[index] = static_cast<std::byte>(index * 7);
        }
        return out;
    }

    /// A kernel client: connects, sends, expects the echo, closes, expects EOF.
    void kernel_client_round_trip(const std::span<const std::byte> payload) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        const sockaddr_in server = address_of(stack_ip, echo_port);
        ASSERT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&server), sizeof(server)), 0) << std::strerror(errno);
        ASSERT_TRUE(send_all(fd, payload));
        std::vector<std::byte> back(payload.size());
        EXPECT_TRUE(recv_all(fd, back)) << "the echo";
        EXPECT_EQ(back, std::vector<std::byte>(payload.begin(), payload.end()));
        ASSERT_EQ(shutdown(fd, SHUT_WR), 0);
        EXPECT_TRUE(recv_eof(fd)) << "the brick's FIN after ours";
        close(fd);
    }

    /// A kernel server: accepts one connection, echoes until EOF, then closes.
    void kernel_server_echo(const int listener, std::size_t expected) {
        pollfd waiting{.fd = listener, .events = POLLIN, .revents = 0};
        const int ready = poll(&waiting, 1, static_cast<int>(patience.count()));
        ASSERT_GT(ready, 0) << "no connection from the stack within " << patience.count()
                            << " ms: " << (ready < 0 ? std::strerror(errno) : "timed out");
        const int fd = accept(listener, nullptr, nullptr);
        ASSERT_GE(fd, 0) << std::strerror(errno);
        std::vector<std::byte> buffer(4096);
        while (expected > 0) {
            const std::size_t want = std::min(expected, buffer.size());
            ASSERT_TRUE(recv_all(fd, std::span<std::byte>{buffer}.first(want)));
            ASSERT_TRUE(send_all(fd, std::span<const std::byte>{buffer}.first(want)));
            expected -= want;
        }
        EXPECT_TRUE(recv_eof(fd)) << "the brick's FIN";
        close(fd);
    }

    /// A kernel socket listening on the kernel's address, or -1 with the failing call reported.
    [[nodiscard]] int kernel_listener() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            ADD_FAILURE() << "socket: " << std::strerror(errno);
            return -1;
        }
        const int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
            ADD_FAILURE() << "setsockopt(SO_REUSEADDR): " << std::strerror(errno);
            close(fd);
            return -1;
        }
        const sockaddr_in local = address_of(kernel_ip, echo_port);
        if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            ADD_FAILURE() << "bind: " << std::strerror(errno);
            close(fd);
            return -1;
        }
        if (listen(fd, 1) != 0) {
            ADD_FAILURE() << "listen: " << std::strerror(errno);
            close(fd);
            return -1;
        }
        return fd;
    }

    /// The environment's `net_tap0`, opened once for the whole suite: a closed DPDK port cannot be reopened, and a
    /// second tap cannot take the interface name the first one holds.
    class TcpTap : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            if (geteuid() != 0) {
                return;
            }
            shared_port().emplace(aloe::ethdev::PortConfig{.name = "net_tap0", .queues = 1, .pool_size = 1024});
            configure_interface(interface, kernel_ip, netmask);
        }

        static void TearDownTestSuite() {
            shared_port().reset();
        }

        void SetUp() override {
            if (geteuid() != 0) {
                GTEST_SKIP() << "needs root for the tap device";
            }
            ASSERT_TRUE(shared_port().has_value());
            port_ = &*shared_port();
        }

        Port* port_ = nullptr;

    private:
        [[nodiscard]] static std::optional<Port>& shared_port() {
            static std::optional<Port> port;
            return port;
        }
    };

}  // namespace

TEST_F(TcpTap, BrickServer) {
    const auto payload = message(5000);  // larger than the MSS, combined with FIN by the kernel at the end
    BrickLoop loop{*port_, true, {}};
    kernel_client_round_trip(payload);
    EXPECT_TRUE(eventually([&] { return loop.clean_close(); }))
        << "the brick saw no clean close within " << patience.count() << " ms: the kernel's last ACK went missing";
    loop.stop();
    EXPECT_EQ(loop.counters().connections_accepted, 1U);
    EXPECT_EQ(loop.counters().connections_closed, 1U) << "both FINs acknowledged";
    EXPECT_EQ(loop.counters().dropped_bad_checksum, 0U);
    EXPECT_EQ(loop.counters().dropped_bad_header, 0U) << "the kernel's options were skipped";
    EXPECT_TRUE(loop.clean_close());
}

TEST_F(TcpTap, BrickClient) {
    const auto payload = message(3000);
    const int listener = kernel_listener();
    ASSERT_GE(listener, 0);
    BrickLoop loop{*port_, false, payload};
    kernel_server_echo(listener, payload.size());
    EXPECT_TRUE(eventually([&] { return loop.clean_close(); }))
        << "the brick saw no clean close within " << patience.count() << " ms";
    loop.stop();
    close(listener);
    EXPECT_EQ(loop.received(), payload);
    EXPECT_EQ(loop.counters().connections_opened, 1U);
    EXPECT_EQ(loop.counters().connections_closed, 1U);
}

TEST_F(TcpTap, RuntimeServer) {
    const auto payload = message(5000);
    std::atomic<bool> listening{false};
    std::atomic<int> served{0};
    std::atomic<int> closed{0};
    Runtime runtime{
        {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
        *port_,
        aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24},
        aloe::tcp::TcpConfig{}
    };
    runtime.start();
    runtime.spawn(0,
                  serve_task(&runtime.shard(0).stack().streams(), runtime.scheduler(0), &listening, &served, &closed));
    ASSERT_TRUE(eventually([&] { return listening.load(std::memory_order_acquire); }))
        << "the runtime never started listening within " << patience.count() << " ms";
    kernel_client_round_trip(payload);
    EXPECT_TRUE(eventually([&] { return closed.load(std::memory_order_acquire) == 1; }))
        << "the runtime's echo saw no clean close within " << patience.count() << " ms";
    runtime.stop();
    runtime.join();
    EXPECT_EQ(served.load(), 1);
    const auto& counters = runtime.shard(0).stack().tcp().counters();
    EXPECT_EQ(counters.connections_closed, 1U);
    EXPECT_EQ(counters.dropped_bad_header, 0U);
}

TEST_F(TcpTap, RuntimeClient) {
    const auto payload = message(3000);
    const int listener = kernel_listener();
    ASSERT_GE(listener, 0);
    std::vector<std::byte> received;
    std::atomic<int> outcome{0};
    Runtime runtime{
        {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
        *port_,
        aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24},
        aloe::tcp::TcpConfig{}
    };
    runtime.start();
    runtime.spawn(0, client_task(&runtime.shard(0).stack().streams(), payload, &received, &outcome));
    kernel_server_echo(listener, payload.size());
    EXPECT_TRUE(eventually([&] { return outcome.load() != 0; }))
        << "the client task did not finish within " << patience.count() << " ms";
    runtime.stop();
    runtime.join();
    close(listener);
    EXPECT_EQ(outcome.load(), 1);
    EXPECT_EQ(received, payload);
    EXPECT_EQ(runtime.shard(0).stack().tcp().counters().connections_closed, 1U);
}
