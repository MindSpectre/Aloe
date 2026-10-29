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

    /// The environment's `net_tap0`, opened once for the whole suite: a closed DPDK port cannot be reopened, and a
    /// second tap cannot take the interface name the first one holds.
    class TcpTap : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            if (geteuid() != 0) {
                return;
            }
            aloe::core::calibrate_clock();  // here, where a refusal is an exception, not on a loop's thread
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
