#include <algorithm>
#include <aloe/core>
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
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <eal_environment.hpp>
#include <fcntl.h>
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
// The kernel listens for the client tests, so the host firewall must accept inbound TCP on the tap: Fedora's
// firewalld puts `aloe-tcp` in its default zone, which opens only ports 1025-65535, hence an echo port above 1024.
//
// The cases of `TcpTapLoss` repeat the four echoes with 50 000 bytes under loss in both directions. Their fixture runs
//
//     tc qdisc replace dev aloe-tcp root netem loss 5%
//
// on the tap, so the kernel loses 5% of what it sends the stack, and removes the qdisc afterwards; it skips when `tc`
// or `sch_netem` is missing. The stack's own transmits go through a `LossyDevice`, which frees every tenth packet and
// reports it sent. Each case needs at least one retransmission on the stack's side and the clean close; the brick
// cases need timestamps negotiated, which the kernel offers by default. Waits are 30 s there. Each case also prints
// how far the kernel's `RetransSegs` (the `Tcp:` rows of `/proc/net/snmp`) rose while it ran: the kernel's own
// resends, for information only, since the counter is system-wide and other traffic moves it too.
//
// `BrickServerProbesAKernelClientThatStopsReading` is the probe against Linux: a kernel client with an 8 KB
// `SO_RCVBUF` sends 50 000 bytes and reads none of the echo until the brick has sent two zero-window probes, so
// the kernel's window closes and the brick probes it, and the kernel's answer to the first probe is seen accepted;
// the connection must stay open, and once the client reads, the echo completes and both sides close cleanly.
//
// The cases of `TcpTapReceive` turn it round: the kernel sends and the brick receives, stepped on the test's thread
// between non-blocking calls on the kernel's socket. They check that SACK and window scaling are negotiated whichever
// end opens, that a kernel sender fills a window past 65535 bytes up to the point where it stalls at the edge, and,
// under
//
//     tc qdisc replace dev aloe-tcp root netem delay 5ms reorder 25% 50%
//     tc qdisc replace dev aloe-tcp root netem loss 2%
//
// that 2 MB arrive byte-exact with the kernel's `tcp_info` showing no retransmission timeout under reordering and
// timeouts at most a quarter of its resends under loss. For those two the tap's `gso_max_segs` is set to 1, so netem
// drops and reorders single segments rather than GSO bursts, and restored afterwards. They read `tcpi_total_rto`
// (Linux 6.7) and skip on an older kernel, or when `ip`, `tc` or `sch_netem` is missing; the qdisc is removed after
// each case. The binary needs `tcpi_total_rto`, `tcpi_snd_wnd` and `tcpi_bytes_acked` in glibc's `<netinet/tcp.h>`,
// which glibc 2.43, the version it was written on, has; an older glibc without them fails to compile it. Run them
// alone, since the earlier suites' loop threads register lcores that are never unregistered and each keeps part of
// the 1024-packet pool in its mempool cache:
//
//     sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap --gtest_filter='TcpTapReceive.*'
//
// The kernel's SYN carries SACK-permitted, timestamps and window scale, which the option parser skips; the tap
// offers no offloads, so every checksum is software on our side and verified by the kernel.
namespace {

    using namespace std::chrono_literals;
    using Port = aloe::ethdev::Port;
    template <typename Device>
    using Ipv4T = aloe::net::Ipv4<Device>;
    template <typename Device>
    using TcpT = aloe::tcp::Stack<Ipv4T<Device>>;
    template <typename Device>
    using StackT = aloe::runtime::TcpStack<Device>;
    template <typename Device>
    using RuntimeT = aloe::runtime::Runtime<Device, StackT<Device>>;

    constexpr std::string_view interface = "aloe-tcp";
    constexpr aloe::wire::Ipv4Address kernel_ip{10, 78, 0, 1};
    constexpr aloe::wire::Ipv4Address stack_ip{10, 78, 0, 2};
    constexpr aloe::wire::Ipv4Address netmask{255, 255, 255, 0};
    constexpr std::uint16_t echo_port  = 7007;
    constexpr auto default_patience    = 5000ms;
    constexpr auto lossy_patience      = 30000ms;
    constexpr std::size_t lossy_size   = 50000;
    constexpr std::uint64_t drop_every = 10;

    /// How long a wait lasts: the cases under loss widen it for their duration.
    [[nodiscard]] std::atomic<std::chrono::milliseconds>& patience_storage() {
        static std::atomic<std::chrono::milliseconds> value{default_patience};
        return value;
    }
    [[nodiscard]] std::chrono::milliseconds patience() {
        return patience_storage().load();
    }

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

    /// Waits until `fd` is readable or `deadline` passes; true when readable. The tap driver points an rt-signal at
    /// its queues, so every frame the kernel writes to the tap interrupts some thread, and `poll` is never restarted
    /// by `SA_RESTART`: an `EINTR` here is a frame arriving, not a failure.
    [[nodiscard]] bool readable_by(const int fd, const std::chrono::steady_clock::time_point deadline) {
        for (;;) {
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
            return ready > 0;
        }
    }

    /// Reads exactly `out.size()` bytes within `patience`, or returns false.
    [[nodiscard]] bool recv_all(const int fd, std::span<std::byte> out) {
        const auto deadline = std::chrono::steady_clock::now() + patience();
        while (!out.empty()) {
            if (!readable_by(fd, deadline)) {
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
        if (!readable_by(fd, std::chrono::steady_clock::now() + patience())) {
            return false;
        }
        return ::recv(fd, one.data(), one.size(), 0) == 0;
    }

    /// Polls `done` until it holds or `patience` runs out; true when it held.
    template <typename Predicate>
    [[nodiscard]] bool eventually(Predicate done) {
        const auto deadline = std::chrono::steady_clock::now() + patience();
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /// The port seen through a device that loses every tenth packet it is handed, as a wire that drops a frame
    /// would: the packet is freed and counted as accepted, so the stack sees a transmit that succeeded.
    template <aloe::device::IsDevice Device>
    class LossyDevice {
    public:
        using Packet = typename Device::Packet;

        explicit LossyDevice(Device& owner) noexcept
            : device_{&owner} {
        }

        [[nodiscard]] std::uint64_t dropped() const noexcept {
            return dropped_;
        }
        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return device_->queue_count();
        }
        [[nodiscard]] aloe::wire::MacAddress mac() const noexcept {
            return device_->mac();
        }
        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return device_->mtu();
        }
        [[nodiscard]] bool link_up() const noexcept {
            return device_->link_up();
        }
        [[nodiscard]] const aloe::device::Capabilities& capabilities() const noexcept {
            return device_->capabilities();
        }
        [[nodiscard]] const aloe::device::RssDescription& steering() const noexcept {
            return device_->steering();
        }
        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            return device_->allocate(queue);
        }
        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
            return device_->receive(queue, out);
        }

        /// Hands the packets over one at a time; every tenth is freed instead. Returns the accepted prefix.
        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
            for (std::size_t index = 0; index < in.size(); ++index) {
                if ((seen_ + 1) % drop_every == 0) {
                    in[index] = Packet{};
                    ++dropped_;
                } else if (device_->transmit(queue, in.subspan(index, 1)) == 0) {
                    return index;
                }
                ++seen_;
            }
            return in.size();
        }

        [[nodiscard]] std::size_t reclaim(const std::uint16_t queue) noexcept {
            return device_->reclaim(queue);
        }
        [[nodiscard]] aloe::device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return device_->counters(queue);
        }

    private:
        Device* device_;
        std::uint64_t seen_    = 0;
        std::uint64_t dropped_ = 0;
    };

    static_assert(aloe::device::IsDevice<LossyDevice<Port>>);

    /// The hand-written echo over the bricks on its own thread: server when `listen`, client to `peer` otherwise.
    template <typename Device>
    class BrickLoop {
        using Ipv4 = Ipv4T<Device>;
        using Tcp  = TcpT<Device>;

    public:
        BrickLoop(Device& port, const bool listen, const std::span<const std::byte> to_send)
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
        [[nodiscard]] bool timestamps_negotiated() const noexcept {
            return timestamps_.load(std::memory_order_acquire);
        }
        /// The stack's `zero_window_probes` as of the loop's last tick: readable while the loop runs.
        [[nodiscard]] std::uint64_t zero_window_probes() const noexcept {
            return zero_window_probes_.load(std::memory_order_acquire);
        }
        /// The connection raised `Closed`, `Reset` or `TimedOut`.
        [[nodiscard]] bool ended() const noexcept {
            return ended_.load(std::memory_order_acquire);
        }
        std::atomic<bool> connected{false};

    private:
        void
        run(Device& port, const bool listen, const std::span<const std::byte> to_send, const std::stop_token& stop) {
            aloe::ethdev::register_thread();
            aloe::loop::ShardCounters shard_counters;
            aloe::loop::ShardQueue<Device> queue{port, 0, 64, shard_counters};
            Ipv4 ip{
                queue, {.address = stack_ip, .prefix = 24}
            };
            aloe::loop::TimerWheel wheel{1ms, aloe::core::Clock::now()};
            Tcp tcp{ip, wheel, {}};
            std::vector<typename Device::Packet> burst(64);
            std::size_t offered = 0;
            if (listen) {
                std::ignore = tcp.listen(echo_port);
            } else {
                std::ignore = tcp.connect({.address = kernel_ip, .port = echo_port}, aloe::core::Clock::now());
            }
            while (!stop.stop_requested()) {
                const auto now             = aloe::core::Clock::now();
                const std::size_t received = queue.receive(burst);
                ip.process(std::span<typename Device::Packet>{burst}.first(received), now);
                tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
                std::ignore = wheel.advance(now);
                while (const auto event = tcp.poll_event()) {
                    auto& c = *event->connection;
                    if (event->events.connected() || event->events.accepted()) {
                        timestamps_.store(c.timestamps(), std::memory_order_release);
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
                        ended_.store(true, std::memory_order_release);
                        c.release();
                    } else if (event->events.reset() || event->events.timed_out()) {
                        ended_.store(true, std::memory_order_release);
                        c.release();
                    }
                }
                tcp.flush(now);
                std::ignore = queue.flush();
                zero_window_probes_.store(tcp.counters().zero_window_probes, std::memory_order_release);
                if (received == 0) {
                    std::this_thread::yield();
                }
            }
            counters_ = tcp.counters();
            queue.discard();
        }

        static void echo(typename Tcp::ConnectionType& c) {
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
        std::atomic<bool> timestamps_{false};
        std::atomic<bool> clean_close_{false};  ///< Written by the loop thread, polled by the test thread.
        std::atomic<bool> ended_{false};
        std::atomic<std::uint64_t> zero_window_probes_{0};
        std::jthread thread_;
    };

    /// The echo as tasks over the runtime.
    template <typename Device>
    aloe::runtime::task<void> echo_task(typename StackT<Device>::StreamType stream, std::atomic<int>* closed) {
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
    template <typename Device>
    aloe::runtime::task<void> serve_task(typename StackT<Device>::StreamsType* streams,
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
            scheduler.spawn(echo_task<Device>(std::move(*stream), closed));
        }
    }

    template <typename Device>
    aloe::runtime::task<void> client_task(typename StackT<Device>::StreamsType* streams,
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

    /// A kernel client that stops reading: an 8 KB receive buffer, set before connecting so the window is sized from
    /// it, and a send buffer that takes the whole payload at once; it sends everything, runs `stalled` with the socket
    /// while the echo is left unread, then reads the echo, closes and expects EOF.
    template <typename Stalled>
    void kernel_client_stalled_round_trip(const std::span<const std::byte> payload, Stalled stalled) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        const int receive_buffer = 8192;
        ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)), 0)
            << std::strerror(errno);
        const int send_buffer = 131072;
        ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)), 0) << std::strerror(errno);
        const sockaddr_in server = address_of(stack_ip, echo_port);
        ASSERT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&server), sizeof(server)), 0) << std::strerror(errno);
        ASSERT_TRUE(send_all(fd, payload));
        stalled(fd);
        std::vector<std::byte> back(payload.size());
        EXPECT_TRUE(recv_all(fd, back)) << "the echo, once the client reads";
        EXPECT_EQ(back, std::vector<std::byte>(payload.begin(), payload.end()));
        ASSERT_EQ(shutdown(fd, SHUT_WR), 0);
        EXPECT_TRUE(recv_eof(fd)) << "the brick's FIN after ours";
        close(fd);
    }

    /// The kernel's `RetransSegs`, from the `Tcp:` rows of `/proc/net/snmp`, a row of names and a row of values;
    /// nothing when the file or the field is missing. System-wide: every TCP socket of the host moves it.
    [[nodiscard]] std::optional<std::uint64_t> kernel_retrans_segs() {
        std::ifstream snmp{"/proc/net/snmp"};
        std::string names;
        std::string values;
        for (std::string line; std::getline(snmp, line);) {
            if (!line.starts_with("Tcp:")) {
                continue;
            }
            if (names.empty()) {
                names = line;
            } else {
                values = line;
                break;
            }
        }
        std::istringstream name_stream{names};
        std::istringstream value_stream{values};
        std::string name;
        std::string value;
        while (name_stream >> name && value_stream >> value) {
            if (name == "RetransSegs") {
                std::uint64_t count     = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
                return error == std::errc{} ? std::optional{count} : std::nullopt;
            }
        }
        return std::nullopt;
    }

    /// A kernel server: accepts one connection, echoes until EOF, then closes.
    void kernel_server_echo(const int listener, std::size_t expected) {
        ASSERT_TRUE(readable_by(listener, std::chrono::steady_clock::now() + patience()))
            << "no connection from the stack within " << patience().count() << " ms";
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

    /// The EAL's `net_tap0`, opened once per process, after the EAL and closed before it: a closed DPDK port cannot
    /// be reopened, and a second tap cannot take the interface name the first one holds. Every suite takes the port
    /// from here: gtest runs a fixture's suite setup and teardown once per suite name, so a port kept by the fixture
    /// would be closed after `TcpTap`'s cases and opened again, in vain, for `TcpTapLoss`'s.
    class TapPortEnvironment : public ::testing::Environment {
    public:
        void SetUp() override {
            if (geteuid() != 0) {
                return;  // every case skips
            }
            aloe::core::calibrate_clock();  // here, where a refusal is an exception, not on a loop's thread
            port().emplace(aloe::ethdev::PortConfig{.name = "net_tap0", .queues = 1, .pool_size = 1024});
            configure_interface(interface, kernel_ip, netmask);
        }

        void TearDown() override {
            port().reset();
        }

        [[nodiscard]] static std::optional<Port>& port() {
            static std::optional<Port> port;
            return port;
        }
    };

    // Registered after the EAL: environments are set up in this order and torn down in reverse.
    const auto* const tap_port = ::testing::AddGlobalTestEnvironment(new TapPortEnvironment{});

    class TcpTap : public ::testing::Test {
    protected:
        void SetUp() override {
            if (geteuid() != 0) {
                GTEST_SKIP() << "needs root for the tap device";
            }
            ASSERT_TRUE(TapPortEnvironment::port().has_value());
            port_ = &*TapPortEnvironment::port();
        }

        Port* port_ = nullptr;
    };

    /// Runs `tc` with `arguments`; true when it ran and succeeded.
    [[nodiscard]] bool tc(const std::string_view arguments) {
        return std::system(("tc " + std::string{arguments} + " >/dev/null 2>&1").c_str()) == 0;
    }

    /// The tap with the kernel's egress to it shaped by netem: 5% of what the kernel sends the stack is lost, and the
    /// cases wrap the port in a `LossyDevice`, which loses every tenth frame the stack sends. Waits are 30 s.
    class TcpTapLoss : public TcpTap {
    protected:
        void SetUp() override {
            TcpTap::SetUp();
            if (IsSkipped() || HasFatalFailure()) {
                return;
            }
            if (!tc("qdisc replace dev aloe-tcp root netem loss 5%")) {
                GTEST_SKIP() << "`tc qdisc replace dev aloe-tcp root netem loss 5%` failed: tc or sch_netem is missing";
            }
            shaped_ = true;
            patience_storage().store(lossy_patience);
            retrans_before_ = kernel_retrans_segs();
        }

        /// Prints how far the kernel's `RetransSegs` rose during the case, and records it as a property: information,
        /// not an assertion, since the counter is system-wide.
        void TearDown() override {
            patience_storage().store(default_patience);
            if (shaped_) {
                std::ignore = tc("qdisc del dev aloe-tcp root");
            }
            const std::optional<std::uint64_t> after = kernel_retrans_segs();
            if (retrans_before_ && after) {
                const std::uint64_t resent = *after - *retrans_before_;
                std::println("the kernel's RetransSegs rose by {} during the case (system-wide)", resent);
                RecordProperty("kernel_retrans_segs", std::to_string(resent));
            }
        }

    private:
        bool shaped_ = false;
        std::optional<std::uint64_t> retrans_before_{};
    };

    template <typename Device>
    void brick_server(Device& device, const std::size_t size, const bool lossy) {
        const auto payload = message(size);
        BrickLoop<Device> loop{device, true, {}};
        kernel_client_round_trip(payload);
        EXPECT_TRUE(eventually([&] { return loop.clean_close(); }))
            << "the brick saw no clean close within " << patience().count()
            << " ms: the kernel's last ACK went missing";
        loop.stop();
        EXPECT_EQ(loop.counters().connections_accepted, 1U);
        EXPECT_EQ(loop.counters().connections_closed, 1U) << "both FINs acknowledged";
        EXPECT_EQ(loop.counters().dropped_bad_checksum, 0U);
        EXPECT_EQ(loop.counters().dropped_bad_header, 0U) << "the kernel's options were skipped";
        EXPECT_TRUE(loop.clean_close());
        EXPECT_TRUE(loop.timestamps_negotiated()) << "the kernel offers timestamps by default";
        if (lossy) {
            EXPECT_GE(loop.counters().retransmits + loop.counters().fast_retransmits, 1U);
        }
    }

    template <typename Device>
    void brick_client(Device& device, const std::size_t size, const bool lossy) {
        const auto payload = message(size);
        const int listener = kernel_listener();
        ASSERT_GE(listener, 0);
        BrickLoop<Device> loop{device, false, payload};
        kernel_server_echo(listener, payload.size());
        EXPECT_TRUE(eventually([&] { return loop.clean_close(); }))
            << "the brick saw no clean close within " << patience().count() << " ms";
        loop.stop();
        close(listener);
        EXPECT_EQ(loop.received(), payload);
        EXPECT_EQ(loop.counters().connections_opened, 1U);
        EXPECT_EQ(loop.counters().connections_closed, 1U);
        EXPECT_TRUE(loop.timestamps_negotiated()) << "the kernel offers timestamps by default";
        if (lossy) {
            EXPECT_TRUE(loop.clean_close());
            EXPECT_GE(loop.counters().retransmits + loop.counters().fast_retransmits, 1U);
        }
    }

    template <typename Device>
    void runtime_server(Device& device, const std::size_t size, const bool lossy) {
        const auto payload = message(size);
        std::atomic<bool> listening{false};
        std::atomic<int> served{0};
        std::atomic<int> closed{0};
        RuntimeT<Device> runtime{
            {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
            device,
            aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24},
            aloe::tcp::TcpConfig{}
        };
        runtime.start();
        runtime.spawn(0,
                      serve_task<Device>(
                          &runtime.shard(0).stack().streams(), runtime.scheduler(0), &listening, &served, &closed));
        ASSERT_TRUE(eventually([&] { return listening.load(std::memory_order_acquire); }))
            << "the runtime never started listening within " << patience().count() << " ms";
        kernel_client_round_trip(payload);
        EXPECT_TRUE(eventually([&] { return closed.load(std::memory_order_acquire) == 1; }))
            << "the runtime's echo saw no clean close within " << patience().count() << " ms";
        runtime.stop();
        runtime.join();
        EXPECT_EQ(served.load(), 1);
        const auto& counters = runtime.shard(0).stack().tcp().counters();
        EXPECT_EQ(counters.connections_closed, 1U);
        EXPECT_EQ(counters.dropped_bad_header, 0U);
        if (lossy) {
            EXPECT_EQ(closed.load(), 1);
            EXPECT_GE(counters.retransmits + counters.fast_retransmits, 1U);
        }
    }

    template <typename Device>
    void runtime_client(Device& device, const std::size_t size, const bool lossy) {
        const auto payload = message(size);
        const int listener = kernel_listener();
        ASSERT_GE(listener, 0);
        std::vector<std::byte> received;
        std::atomic<int> outcome{0};
        RuntimeT<Device> runtime{
            {.shard = {}, .threads = {}, .thread_hook = aloe::ethdev::register_thread},
            device,
            aloe::net::Ipv4Config{.address = stack_ip, .prefix = 24},
            aloe::tcp::TcpConfig{}
        };
        runtime.start();
        runtime.spawn(0, client_task<Device>(&runtime.shard(0).stack().streams(), payload, &received, &outcome));
        kernel_server_echo(listener, payload.size());
        EXPECT_TRUE(eventually([&] { return outcome.load() != 0; }))
            << "the client task did not finish within " << patience().count() << " ms";
        runtime.stop();
        runtime.join();
        close(listener);
        EXPECT_EQ(outcome.load(), 1);
        EXPECT_EQ(received, payload);
        const auto& counters = runtime.shard(0).stack().tcp().counters();
        EXPECT_EQ(counters.connections_closed, 1U);
        if (lossy) {
            EXPECT_GE(counters.retransmits + counters.fast_retransmits, 1U);
        }
    }

    /// `size` bytes in which every four-byte word holds its own index: no period a misplaced segment could hide in, as
    /// `message`'s 256-byte one could for segments 32 full segments apart.
    [[nodiscard]] std::vector<std::byte> numbered_words(const std::size_t size) {
        std::vector<std::byte> out(size);
        for (std::size_t index = 0; index < size; ++index) {
            const std::size_t word = index / 4;
            const std::size_t lane = index % 4;
            out[index]             = static_cast<std::byte>(word >> (8 * lane));
        }
        return out;
    }

    /// Fails with the first offset at which `read` and `sent` differ, and with both sizes when they differ; byte-exact.
    void expect_same_bytes(const std::span<const std::byte> read, const std::span<const std::byte> sent) {
        EXPECT_EQ(read.size(), sent.size()) << "the bytes the stack read against the bytes the kernel sent";
        const auto [mine, theirs] = std::ranges::mismatch(read, sent);
        if (mine != read.end() && theirs != sent.end()) {
            ADD_FAILURE() << "the stack read 0x" << std::hex << std::to_integer<unsigned>(*mine)
                          << " where the kernel sent 0x" << std::to_integer<unsigned>(*theirs) << std::dec
                          << ", first at offset " << (mine - read.begin()) << " of " << sent.size();
        }
    }

    /// A kernel socket closed when it goes out of scope, on every path out of a case.
    class KernelSocket {
    public:
        explicit KernelSocket(const int fd) noexcept
            : fd_{fd} {
        }
        KernelSocket(const KernelSocket&)            = delete;
        KernelSocket& operator=(const KernelSocket&) = delete;
        KernelSocket(KernelSocket&&)                 = delete;
        KernelSocket& operator=(KernelSocket&&)      = delete;
        ~KernelSocket() {
            if (fd_ >= 0) {
                close(fd_);
            }
        }

        [[nodiscard]] int fd() const noexcept {
            return fd_;
        }

    private:
        int fd_ = -1;
    };

    /// True when `fd` has one of `events` now; `poll` with no wait, retried on the tap's `EINTR`.
    [[nodiscard]] bool ready_now(const int fd, const short events) {
        for (;;) {
            pollfd waiting{.fd = fd, .events = events, .revents = 0};
            const int ready = poll(&waiting, 1, 0);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            return ready > 0;
        }
    }

    /// The kernel socket's `struct tcp_info`, and how much of it the running kernel filled in: an older kernel returns
    /// a shorter structure than this build's header describes.
    struct KernelTcpInfo {
        tcp_info info{};
        socklen_t length = 0;

        /// The running kernel filled in the field that ends at `end`.
        [[nodiscard]] bool reports(const std::size_t end) const noexcept {
            return length >= end;
        }
    };

    // Where the fields the cases read end. `tcpi_options` and the byte of the two window-scale bitfields come before
    // `tcpi_rto`; `tcpi_total_rto` arrived in Linux 6.7, `tcpi_snd_wnd` some releases before it.
    constexpr std::size_t tcp_info_options_end = offsetof(tcp_info, tcpi_rto);
    constexpr std::size_t tcp_info_unacked_end = offsetof(tcp_info, tcpi_unacked) + sizeof(tcp_info::tcpi_unacked);
    constexpr std::size_t tcp_info_bytes_acked_end =
        offsetof(tcp_info, tcpi_bytes_acked) + sizeof(tcp_info::tcpi_bytes_acked);
    constexpr std::size_t tcp_info_snd_wnd_end = offsetof(tcp_info, tcpi_snd_wnd) + sizeof(tcp_info::tcpi_snd_wnd);
    constexpr std::size_t tcp_info_total_rto_end =
        offsetof(tcp_info, tcpi_total_rto) + sizeof(tcp_info::tcpi_total_rto);

    /// `getsockopt(fd, IPPROTO_TCP, TCP_INFO)`, or nothing with the failure reported.
    [[nodiscard]] std::optional<KernelTcpInfo> kernel_tcp_info(const int fd) {
        KernelTcpInfo out{};
        out.length = sizeof(out.info);
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &out.info, &out.length) != 0) {
            ADD_FAILURE() << "getsockopt(TCP_INFO): " << std::strerror(errno);
            return std::nullopt;
        }
        return out;
    }

    /// The receive side over the bricks, stepped on the test's own thread between the kernel socket's calls, which
    /// never block. The stack listens on `echo_port` or connects to the kernel there; it takes what arrives into
    /// `received()` only while reading is on, and closes once the kernel's FIN is in and everything is read.
    ///
    /// Unlike `BrickLoop` it runs no thread of its own. The test's thread is the EAL's main lcore, so every case takes
    /// its packets through the same mempool cache; a registered thread per case would leave the packets its cache
    /// holds out of the 1024-packet pool's reach when it exits, since no lcore is ever unregistered, and these cases
    /// hold hundreds of packets at once.
    class ReceiveSide {
    public:
        using Tcp        = TcpT<Port>;
        using Connection = Tcp::ConnectionType;

        ReceiveSide(Port& port, const bool listen)
            : queue_{
                  port, 0, 64, shard_counters_
        },
              ip_{queue_, {.address = stack_ip, .prefix = 24}}, wheel_{1ms, aloe::core::Clock::now()},
              tcp_{ip_, wheel_, {}}, burst_(Port::max_burst) {
            if (listen) {
                EXPECT_TRUE(tcp_.listen(echo_port).has_value()) << "the stack's listen on " << echo_port;
            } else {
                EXPECT_TRUE(
                    tcp_.connect({.address = kernel_ip, .port = echo_port}, aloe::core::Clock::now()).has_value())
                    << "the stack's connect to the kernel's " << echo_port;
            }
        }

        ReceiveSide(const ReceiveSide&)            = delete;
        ReceiveSide& operator=(const ReceiveSide&) = delete;
        ReceiveSide(ReceiveSide&&)                 = delete;
        ReceiveSide& operator=(ReceiveSide&&)      = delete;
        ~ReceiveSide() {
            queue_.discard();
        }

        /// One turn of the loop: receive, process, fire timers, take events, read when reading, flush.
        void step() {
            const auto now             = aloe::core::Clock::now();
            const std::size_t received = queue_.receive(burst_);
            ip_.process(std::span<Port::Packet>{burst_}.first(received), now);
            tcp_.process(ip_.received(aloe::wire::Ipv4Protocol::Tcp), now);
            std::ignore = wheel_.advance(now);
            while (const auto event = tcp_.poll_event()) {
                auto& c = *event->connection;
                if (event->events.connected() || event->events.accepted()) {
                    connection_ = &c;
                }
                if (event->events.closed() || event->events.reset() || event->events.timed_out()) {
                    clean_close_ =
                        event->events.closed() && !event->events.reset() && c.state() == aloe::tcp::State::Closed;
                    ended_ = true;
                    if (connection_ == &c) {
                        connection_ = nullptr;
                    }
                    c.release();
                }
            }
            if (connection_ != nullptr) {
                Connection& c = *connection_;
                peak_unread_  = std::max(peak_unread_, c.unread().size());
                if (reading_) {
                    for (const std::span<const std::byte> chunk : c.unread()) {
                        received_.insert(received_.end(), chunk.begin(), chunk.end());
                    }
                    c.consume(c.unread().size());
                    if (c.peer_closed() && c.state() == aloe::tcp::State::CloseWait) {
                        c.close();
                    }
                }
            }
            tcp_.flush(now);
            std::ignore = queue_.flush();
            if (received == 0) {
                std::this_thread::yield();
            }
        }

        void read(const bool on) noexcept {
            reading_ = on;
        }
        /// The open connection, from its `Connected` or `Accepted` event until its end; null otherwise.
        [[nodiscard]] const Connection* connection() const noexcept {
            return connection_;
        }
        [[nodiscard]] std::size_t unread() const noexcept {
            return connection_ == nullptr ? 0 : connection_->unread().size();
        }
        /// The most the connection held unread at the end of a turn.
        [[nodiscard]] std::size_t peak_unread() const noexcept {
            return peak_unread_;
        }
        [[nodiscard]] const std::vector<std::byte>& received() const noexcept {
            return received_;
        }
        [[nodiscard]] const aloe::tcp::TcpCounters& counters() const noexcept {
            return tcp_.counters();
        }
        /// The connection ended by `Closed`, both FINs acknowledged, with no reset.
        [[nodiscard]] bool clean_close() const noexcept {
            return clean_close_;
        }
        /// The connection raised `Closed`, `Reset` or `TimedOut`.
        [[nodiscard]] bool ended() const noexcept {
            return ended_;
        }

    private:
        aloe::loop::ShardCounters shard_counters_;
        aloe::loop::ShardQueue<Port> queue_;
        Ipv4T<Port> ip_;
        aloe::loop::TimerWheel wheel_;
        Tcp tcp_;
        std::vector<Port::Packet> burst_;
        Connection* connection_ = nullptr;
        std::vector<std::byte> received_;
        std::size_t peak_unread_ = 0;
        bool reading_            = false;
        bool clean_close_        = false;
        bool ended_              = false;
    };

    /// Steps `side` until `done()` holds or `patience` runs out; true when it held. `done` runs once per turn, so it
    /// is also where a case drives the kernel's socket.
    template <typename Done>
    [[nodiscard]] bool step_until(ReceiveSide& side, Done done) {
        const auto deadline = std::chrono::steady_clock::now() + patience();
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            side.step();
        }
        return true;
    }

    /// The kernel end as the sender: hands the socket what it takes without blocking, once per turn of the stack.
    class KernelSender {
    public:
        KernelSender(const int fd, const std::span<const std::byte> payload) noexcept
            : fd_{fd},
              payload_{payload} {
        }

        /// Sends up to `limit` bytes of the payload in all, as many as the socket's buffer takes now. False once a
        /// send fails with anything but a full buffer, which `error()` then names.
        [[nodiscard]] bool pump(const std::size_t limit = std::numeric_limits<std::size_t>::max()) noexcept {
            const std::size_t end = std::min(limit, payload_.size());
            while (error_ == 0 && sent_ < end) {
                const ssize_t sent = ::send(fd_, payload_.data() + sent_, end - sent_, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent >= 0) {
                    sent_ += static_cast<std::size_t>(sent);
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno != EINTR) {
                    error_ = errno;
                }
            }
            return error_ == 0;
        }

        [[nodiscard]] int fd() const noexcept {
            return fd_;
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return payload_.size();
        }
        /// Bytes the kernel's socket has taken so far.
        [[nodiscard]] std::size_t sent() const noexcept {
            return sent_;
        }
        [[nodiscard]] int error() const noexcept {
            return error_;
        }

    private:
        int fd_ = -1;
        std::span<const std::byte> payload_;
        std::size_t sent_ = 0;
        int error_        = 0;
    };

    /// A non-blocking kernel socket connected to the stack's `echo_port`, the stack stepped while the handshake runs;
    /// -1 with the failure reported when it does not connect within `patience`. A `send_buffer` above zero is set as
    /// `SO_SNDBUF` before connecting, which also stops the kernel growing the buffer on its own.
    [[nodiscard]] int kernel_connect(ReceiveSide& side, const int send_buffer = 0) {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            ADD_FAILURE() << "socket: " << std::strerror(errno);
            return -1;
        }
        if (send_buffer > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) != 0) {
            ADD_FAILURE() << "setsockopt(SO_SNDBUF): " << std::strerror(errno);
            close(fd);
            return -1;
        }
        const sockaddr_in server = address_of(stack_ip, echo_port);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&server), sizeof(server)) != 0 && errno != EINPROGRESS) {
            ADD_FAILURE() << "connect: " << std::strerror(errno);
            close(fd);
            return -1;
        }
        if (!step_until(side, [fd] { return ready_now(fd, POLLOUT); })) {
            ADD_FAILURE() << "the kernel's connect to the stack did not finish within " << patience().count() << " ms";
            close(fd);
            return -1;
        }
        int error            = 0;
        socklen_t error_size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error != 0) {
            ADD_FAILURE() << "the kernel's connect to the stack failed: " << std::strerror(error != 0 ? error : errno);
            close(fd);
            return -1;
        }
        return fd;
    }

    /// The connection the stack opened to `listener`, accepted non-blocking with the stack stepped meanwhile; -1 with
    /// the failure reported when none arrives within `patience`.
    [[nodiscard]] int kernel_accept(ReceiveSide& side, const int listener) {
        if (!step_until(side, [listener] { return ready_now(listener, POLLIN); })) {
            ADD_FAILURE() << "no connection from the stack within " << patience().count() << " ms";
            return -1;
        }
        const int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0) {
            ADD_FAILURE() << "accept4: " << std::strerror(errno);
        }
        return fd;
    }

    /// The kernel sends the rest of `sender`'s payload while the stack reads it. Once the stack has read all of it and
    /// the kernel has nothing left unacknowledged, returns the kernel socket's `tcp_info`: read then, it counts the
    /// transfer's resends and timeouts and not those of the FIN that follows. Nothing, with the failure reported, when
    /// either does not happen within `patience`.
    [[nodiscard]] std::optional<KernelTcpInfo> kernel_transfer(ReceiveSide& side, KernelSender& sender) {
        side.read(true);
        const bool delivered =
            step_until(side, [&] { return !sender.pump() || side.received().size() >= sender.size(); });
        if (sender.error() != 0) {
            ADD_FAILURE() << "the kernel's send failed after " << sender.sent()
                          << " bytes: " << std::strerror(sender.error());
            return std::nullopt;
        }
        if (!delivered) {
            ADD_FAILURE() << "the stack read " << side.received().size() << " of " << sender.size() << " bytes within "
                          << patience().count() << " ms; the kernel's socket had taken " << sender.sent();
            return std::nullopt;
        }
        std::optional<KernelTcpInfo> info;
        const bool acknowledged = step_until(side, [&] {
            info = kernel_tcp_info(sender.fd());
            return !info || !info->reports(tcp_info_unacked_end) || info->info.tcpi_unacked == 0;
        });
        if (!info) {
            return std::nullopt;
        }
        if (!info->reports(tcp_info_unacked_end)) {
            ADD_FAILURE() << "the kernel's tcp_info is " << info->length << " bytes, short of tcpi_unacked";
            return std::nullopt;
        }
        if (!acknowledged) {
            ADD_FAILURE() << "the kernel still had " << info->info.tcpi_unacked << " segments unacknowledged "
                          << patience().count() << " ms after the stack read everything";
            return std::nullopt;
        }
        return info;
    }

    /// The kernel closes its side; the stack, which has read everything, closes too, and the kernel reads EOF. Then
    /// the stack's bytes are compared with the payload.
    void kernel_finishes(ReceiveSide& side, const int fd, const std::span<const std::byte> payload) {
        side.read(true);
        ASSERT_EQ(shutdown(fd, SHUT_WR), 0) << std::strerror(errno);
        bool eof = false;
        EXPECT_TRUE(step_until(side,
                               [&] {
                                   if (!eof) {
                                       std::array<std::byte, 1> one{};
                                       eof = ::recv(fd, one.data(), one.size(), MSG_DONTWAIT) == 0;
                                   }
                                   return eof && side.ended();
                               }))
            << "within " << patience().count() << " ms: the kernel read EOF " << eof
            << ", the stack's connection ended " << side.ended();
        EXPECT_TRUE(side.clean_close()) << "both FINs acknowledged, no reset";
        expect_same_bytes(side.received(), payload);
    }

    /// One handshake, the kernel connecting or listening on `echo_port` (above 1024, for firewalld), both ends'
    /// options compared, then 100 000 bytes from the kernel through the scaled window and a clean close.
    void negotiation(Port& port, const bool kernel_connects) {
        SCOPED_TRACE(kernel_connects ? "the kernel connects" : "the kernel listens");
        const auto payload = numbered_words(100000);
        const KernelSocket listener{kernel_connects ? -1 : kernel_listener()};
        if (!kernel_connects) {
            ASSERT_GE(listener.fd(), 0);
            ASSERT_EQ(fcntl(listener.fd(), F_SETFL, O_NONBLOCK), 0) << std::strerror(errno);
        }
        ReceiveSide side{port, kernel_connects};
        const KernelSocket kernel{kernel_connects ? kernel_connect(side) : kernel_accept(side, listener.fd())};
        ASSERT_GE(kernel.fd(), 0);
        ASSERT_TRUE(step_until(side, [&side] { return side.connection() != nullptr; }))
            << "the stack raised no Connected or Accepted within " << patience().count() << " ms";

        const auto& c = *side.connection();
        EXPECT_TRUE(c.sack()) << "the stack uses SACK";
        EXPECT_TRUE(c.window_scaling()) << "the stack uses window scaling";
        EXPECT_EQ(static_cast<unsigned>(c.window_shift()), 3U) << "the stack's shift with the defaults";
        EXPECT_TRUE(c.timestamps()) << "the kernel offers timestamps by default";
        EXPECT_EQ(c.receive_budget(), aloe::tcp::TcpConfig{}.receive_segments * c.mss() / 2)
            << "half of what the default 512 slots of mss() " << c.mss() << " hold";

        const std::optional<KernelTcpInfo> info = kernel_tcp_info(kernel.fd());
        ASSERT_TRUE(info.has_value());
        ASSERT_TRUE(info->reports(tcp_info_options_end))
            << "the kernel's tcp_info is " << info->length << " bytes, short of tcpi_options";
        const unsigned options    = info->info.tcpi_options;
        const unsigned snd_wscale = info->info.tcpi_snd_wscale;
        const unsigned rcv_wscale = info->info.tcpi_rcv_wscale;
        EXPECT_NE(options & TCPI_OPT_SACK, 0U)
            << "the kernel's tcpi_options 0x" << std::hex << options << " lacks SACK";
        EXPECT_NE(options & TCPI_OPT_WSCALE, 0U)
            << "the kernel's tcpi_options 0x" << std::hex << options << " lacks window scaling";
        EXPECT_EQ(snd_wscale, 3U) << "the kernel's tcpi_snd_wscale, the shift it read from the stack";
        EXPECT_EQ(static_cast<unsigned>(c.peer_window_shift()), rcv_wscale)
            << "the stack's peer_window_shift() against the kernel's tcpi_rcv_wscale";

        KernelSender sender{kernel.fd(), payload};
        EXPECT_TRUE(kernel_transfer(side, sender).has_value());
        kernel_finishes(side, kernel.fd(), payload);
        EXPECT_EQ(side.counters().dropped_out_of_window, 0U) << "the kernel honoured the scaled window";
    }

    /// Runs `ip link set dev aloe-tcp <arguments>`; true when it ran and succeeded.
    [[nodiscard]] bool ip_link_set(const std::string_view arguments) {
        return std::system(("ip link set dev aloe-tcp " + std::string{arguments} + " >/dev/null 2>&1").c_str()) == 0;
    }

    /// The tap's `gso_max_segs` as sysfs states it, or nothing when it cannot be read.
    [[nodiscard]] std::optional<std::string> tap_gso_max_segs() {
        std::ifstream file{"/sys/class/net/aloe-tcp/gso_max_segs"};
        std::string value;
        if (file >> value) {
            return value;
        }
        return std::nullopt;
    }

    /// The receive side against a kernel sender. The netem cases shape the kernel's egress to the tap, which is what
    /// the kernel sends the stack, widen the waits to 30 s, and have the qdisc removed after the case, on every path.
    ///
    /// They also cap the tap's `gso_max_segs` at 1 for the case. netem sees the kernel's skbs before software GSO
    /// splits them, and at the tap's round-trip time TCP builds them close to 64 KB, about 44 segments each, so 2 MB
    /// would be some 45 skbs: `loss 2%` would drop whole bursts, and lose nothing at all in about two runs in five, and
    /// `reorder` would move bursts rather than segments. With the cap every skb is one segment, as on a wire. A socket
    /// reads the cap when it connects, so `shape` runs before the kernel's socket exists.
    class TcpTapReceive : public TcpTap {
    protected:
        /// Caps the tap's GSO at one segment, then `tc qdisc replace dev aloe-tcp root netem <netem>`. Returns why it
        /// could not, which the case reports as a skip, or an empty string when both took effect.
        [[nodiscard]] std::string shape(const std::string_view netem) {
            gso_max_segs_ = tap_gso_max_segs();
            if (!gso_max_segs_) {
                return "/sys/class/net/aloe-tcp/gso_max_segs cannot be read";
            }
            if (!ip_link_set("gso_max_segs 1")) {
                gso_max_segs_.reset();
                return "`ip link set dev aloe-tcp gso_max_segs 1` failed: ip is missing or too old, or the kernel "
                       "cannot "
                       "change gso_max_segs over netlink";
            }
            const std::string command = "qdisc replace dev aloe-tcp root netem " + std::string{netem};
            if (!tc(command)) {
                return "`tc " + command + "` failed: tc or sch_netem is missing";
            }
            shaped_ = true;
            patience_storage().store(lossy_patience);
            return {};
        }

        /// Restores the tap's GSO cap, then removes the qdisc; either failing is reported, since it leaves the tap
        /// changed for the cases after.
        void TearDown() override {
            patience_storage().store(default_patience);
            if (gso_max_segs_) {
                EXPECT_TRUE(ip_link_set("gso_max_segs " + *gso_max_segs_))
                    << "`ip link set dev aloe-tcp gso_max_segs " << *gso_max_segs_
                    << "` failed: the tap keeps a cap of 1";
            }
            if (shaped_) {
                EXPECT_TRUE(tc("qdisc del dev aloe-tcp root"))
                    << "`tc qdisc del dev aloe-tcp root` failed: remove the netem qdisc by hand";
            }
        }

    private:
        std::optional<std::string> gso_max_segs_{};  ///< The value to restore, once the cap is set.
        bool shaped_ = false;
    };

    /// The kernel's resend and timeout counts from `tcp_info`, printed and recorded as properties for the run's record.
    void report_kernel_counts(const KernelTcpInfo& info, const aloe::tcp::TcpCounters& stack) {
        std::println("the kernel: tcpi_total_retrans {}, tcpi_total_rto {}; the stack: out_of_order_held {}, "
                     "sack_acks_sent {}, duplicate_sacks_sent {}, dropped_out_of_order {}",
                     info.info.tcpi_total_retrans,
                     info.info.tcpi_total_rto,
                     stack.out_of_order_held,
                     stack.sack_acks_sent,
                     stack.duplicate_sacks_sent,
                     stack.dropped_out_of_order);
        ::testing::Test::RecordProperty("kernel_total_retrans", std::to_string(info.info.tcpi_total_retrans));
        ::testing::Test::RecordProperty("kernel_total_rto", std::to_string(info.info.tcpi_total_rto));
    }

}  // namespace

TEST_F(TcpTap, BrickServer) {
    brick_server(*port_, 5000, false);  // larger than the MSS, combined with FIN by the kernel at the end
}

TEST_F(TcpTap, BrickClient) {
    brick_client(*port_, 3000, false);
}

TEST_F(TcpTap, RuntimeServer) {
    runtime_server(*port_, 5000, false);
}

TEST_F(TcpTap, RuntimeClient) {
    runtime_client(*port_, 3000, false);
}

TEST_F(TcpTap, BrickServerProbesAKernelClientThatStopsReading) {
    const auto payload = message(lossy_size);
    BrickLoop<Port> loop{*port_, true, {}};
    kernel_client_stalled_round_trip(payload, [&loop](const int fd) {
        EXPECT_TRUE(eventually([&loop] { return loop.zero_window_probes() >= 2; }))
            << "the brick sent fewer than two zero-window probes within " << patience().count()
            << " ms: the kernel's window never closed, or its answer to the first probe was not accepted";
        EXPECT_FALSE(loop.ended()) << "the connection stays open while the kernel's window is closed";
        int error            = 0;
        socklen_t error_size = sizeof(error);
        EXPECT_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size), 0) << std::strerror(errno);
        EXPECT_EQ(error, 0) << "the kernel's socket is still connected: " << std::strerror(error);
    });
    EXPECT_TRUE(eventually([&loop] { return loop.clean_close(); }))
        << "the brick saw no clean close within " << patience().count() << " ms";
    loop.stop();
    EXPECT_EQ(loop.counters().connections_accepted, 1U);
    EXPECT_EQ(loop.counters().connections_closed, 1U) << "both FINs acknowledged";
    EXPECT_EQ(loop.counters().connections_timed_out, 0U);
    EXPECT_GE(loop.counters().zero_window_probes, 2U);
    EXPECT_TRUE(loop.clean_close());
}

TEST_F(TcpTapLoss, BrickServer) {
    LossyDevice<Port> device{*port_};
    brick_server(device, lossy_size, true);
    EXPECT_GE(device.dropped(), 1U);
}

TEST_F(TcpTapLoss, BrickClient) {
    LossyDevice<Port> device{*port_};
    brick_client(device, lossy_size, true);
    EXPECT_GE(device.dropped(), 1U);
}

TEST_F(TcpTapLoss, RuntimeServer) {
    LossyDevice<Port> device{*port_};
    runtime_server(device, lossy_size, true);
    EXPECT_GE(device.dropped(), 1U);
}

TEST_F(TcpTapLoss, RuntimeClient) {
    LossyDevice<Port> device{*port_};
    runtime_client(device, lossy_size, true);
    EXPECT_GE(device.dropped(), 1U);
}

TEST_F(TcpTapReceive, TheKernelNegotiatesBothOptionsInBothDirections) {
    negotiation(*port_, true);
    negotiation(*port_, false);
}

TEST_F(TcpTapReceive, AKernelSenderFillsAWindowPast65535Bytes) {
    const auto payload = numbered_words(std::size_t{1} << 20);
    ReceiveSide side{*port_, true};
    // A send buffer the kernel doubles to 256 KB and no longer grows: with the window's 370688 bytes it takes well
    // under the megabyte, so the kernel is left with bytes to send when the window closes, and a stall is seen as one.
    const KernelSocket kernel{kernel_connect(side, 131072)};
    ASSERT_GE(kernel.fd(), 0);
    ASSERT_TRUE(step_until(side, [&side] { return side.connection() != nullptr; }))
        << "the stack raised no Accepted within " << patience().count() << " ms";
    const auto& c                     = *side.connection();
    const std::uint32_t budget        = c.receive_budget();
    const std::size_t mss             = c.mss();
    const std::size_t slots_hold      = aloe::tcp::TcpConfig{}.receive_segments * mss;
    std::optional<KernelTcpInfo> info = kernel_tcp_info(kernel.fd());
    ASSERT_TRUE(info.has_value());
    if (!info->reports(tcp_info_snd_wnd_end) || !info->reports(tcp_info_bytes_acked_end)) {
        GTEST_SKIP() << "the running kernel's tcp_info is " << info->length << " bytes and stops before tcpi_snd_wnd";
    }

    // At the start: once the stack has acknowledged the first bytes. Before that the kernel knows only the SYN-ACK's
    // field, which is never scaled and states 65535.
    constexpr std::size_t first = 1000;
    KernelSender sender{kernel.fd(), payload};
    ASSERT_TRUE(step_until(side,
                           [&] {
                               if (!sender.pump(first)) {
                                   return true;
                               }
                               info = kernel_tcp_info(kernel.fd());
                               return !info || info->info.tcpi_bytes_acked >= first;
                           }))
        << "the stack did not acknowledge the first " << first << " bytes within " << patience().count() << " ms";
    ASSERT_EQ(sender.error(), 0) << std::strerror(sender.error());
    ASSERT_TRUE(info.has_value());
    EXPECT_GT(info->info.tcpi_snd_wnd, 65535U)
        << "the kernel's tcpi_snd_wnd once the stack acknowledged the first " << first
        << " bytes: the scaled window it reconstructs, against the stack's budget " << budget;

    // The stack does not read; the kernel writes until it stalls at the window's edge. Two signals must agree: the
    // kernel still has bytes to send, and nothing new has arrived for `quiet` while the stack's window is below one
    // segment. `quiet` stays near Linux's first zero-window probe, its RTO of 200 ms or more, so few probes fire.
    constexpr auto quiet = 200ms;
    std::size_t last     = side.unread();
    auto last_change     = std::chrono::steady_clock::now();
    const bool stalled   = step_until(side, [&] {
        if (!sender.pump()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (side.unread() != last) {
            last        = side.unread();
            last_change = now;
            return false;
        }
        return sender.sent() < payload.size() && c.receive_window() < mss && now - last_change >= quiet;
    });
    ASSERT_EQ(sender.error(), 0) << std::strerror(sender.error());
    info = kernel_tcp_info(kernel.fd());
    ASSERT_TRUE(info.has_value());
    const std::size_t at_stall = side.unread();
    EXPECT_TRUE(stalled) << "the kernel did not stall at the window within " << patience().count()
                         << " ms: the stack holds " << at_stall << " bytes unread and offers " << c.receive_window()
                         << ", the kernel's socket took " << sender.sent() << " of " << payload.size()
                         << ", its tcpi_snd_wnd is " << info->info.tcpi_snd_wnd;
    EXPECT_GT(at_stall, 65535U) << "the bytes unread at the stall, past what an unscaled window could offer";
    EXPECT_GE(at_stall, budget - 2 * mss)
        << "the bytes unread at the stall against the budget " << budget << " less two segments of " << mss
        << ": a sender leaves less than a segment unused and the last offer may trail by one; the kernel's "
           "tcpi_snd_wnd "
        << info->info.tcpi_snd_wnd;
    EXPECT_TRUE(side.received().empty()) << "nothing read until now";

    // Then the stack reads everything: the window reopens and the kernel finishes, within `patience`.
    ASSERT_TRUE(kernel_transfer(side, sender).has_value());
    kernel_finishes(side, kernel.fd(), payload);
    // The edge is rounded up to the scale's unit as each segment leaves and may creep past the budget, so what bounds
    // the bytes held is the packet cap, `receive_segments` full segments, not the budget.
    EXPECT_LE(side.peak_unread(), slots_hold)
        << "the most the stack held unread, against what the packet cap's " << aloe::tcp::TcpConfig{}.receive_segments
        << " slots of " << mss << " bytes hold";
    // Linux's zero-window probe is a bare segment at RCV.NXT - 1, which `accept_sequence` counts as
    // `dropped_out_of_window` (and a retransmitted tail as `dropped_duplicate`), so neither is asserted. A probe into
    // a window that is small but open carries new data that fits and is accepted in order. None of the three below
    // can move: nothing is lost on the tap, so nothing arrives above a gap, and at most a few probes add to the slots
    // and nodes the window's segments hold, far below the cap of 512 and the pool.
    EXPECT_EQ(side.counters().dropped_out_of_order, 0U) << "nothing is lost on the tap, so nothing waits above a gap";
    EXPECT_EQ(side.counters().dropped_no_slot, 0U) << "the window, not the packet cap, stopped the kernel";
    EXPECT_EQ(side.counters().dropped_no_node, 0U) << "the window, not the node pool, stopped the kernel";
}

TEST_F(TcpTapReceive, UnderNetemReorderingTheKernelSeesNoTimeout) {
    if (const std::string refused = shape("delay 5ms reorder 25% 50%"); !refused.empty()) {
        GTEST_SKIP() << refused;
    }
    const auto payload = numbered_words(std::size_t{2} << 20);
    ReceiveSide side{*port_, true};
    const KernelSocket kernel{kernel_connect(side)};
    ASSERT_GE(kernel.fd(), 0);
    const std::optional<KernelTcpInfo> before = kernel_tcp_info(kernel.fd());
    ASSERT_TRUE(before.has_value());
    if (!before->reports(tcp_info_total_rto_end)) {
        GTEST_SKIP() << "the running kernel's tcp_info is " << before->length
                     << " bytes and stops before tcpi_total_rto (Linux 6.7)";
    }

    KernelSender sender{kernel.fd(), payload};
    const std::optional<KernelTcpInfo> after = kernel_transfer(side, sender);
    ASSERT_TRUE(after.has_value());
    report_kernel_counts(*after, side.counters());
    kernel_finishes(side, kernel.fd(), payload);
    EXPECT_GT(side.counters().out_of_order_held, 0U) << "netem reordered nothing the stack held above a gap";
    EXPECT_EQ(after->info.tcpi_total_rto, 0U)
        << "the kernel's tcpi_total_rto under reordering alone; its tcpi_total_retrans "
        << after->info.tcpi_total_retrans << ", the stack's sack_acks_sent " << side.counters().sack_acks_sent;
}

TEST_F(TcpTapReceive, UnderNetemLossTheKernelRecoversFromTheBlocks) {
    if (const std::string refused = shape("loss 2%"); !refused.empty()) {
        GTEST_SKIP() << refused;
    }
    const auto payload = numbered_words(std::size_t{2} << 20);
    ReceiveSide side{*port_, true};
    const KernelSocket kernel{kernel_connect(side)};
    ASSERT_GE(kernel.fd(), 0);
    const std::optional<KernelTcpInfo> before = kernel_tcp_info(kernel.fd());
    ASSERT_TRUE(before.has_value());
    if (!before->reports(tcp_info_total_rto_end)) {
        GTEST_SKIP() << "the running kernel's tcp_info is " << before->length
                     << " bytes and stops before tcpi_total_rto (Linux 6.7)";
    }

    KernelSender sender{kernel.fd(), payload};
    const std::optional<KernelTcpInfo> after = kernel_transfer(side, sender);
    ASSERT_TRUE(after.has_value());
    report_kernel_counts(*after, side.counters());
    kernel_finishes(side, kernel.fd(), payload);
    const unsigned retrans = after->info.tcpi_total_retrans;
    const unsigned rto     = after->info.tcpi_total_rto;
    EXPECT_GT(side.counters().sack_acks_sent, 0U) << "the stack sent no ACK with SACK blocks under 2% loss";
    EXPECT_GT(retrans, 0U) << "the kernel's tcpi_total_retrans under 2% loss";
    // A lost retransmission or a lost tail costs the kernel a timeout whatever the receiver does, hence a share.
    // `tcpi_total_rto` also counts a SYN's timeout, a SYN or an ARP exchange that netem lost; the quarter covers it.
    EXPECT_LE(4U * rto, retrans) << "the kernel's tcpi_total_rto " << rto << " is more than a quarter of its "
                                 << "tcpi_total_retrans " << retrans << ": it waited out its timer, not the blocks";
}
