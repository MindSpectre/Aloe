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
#include <functional>
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
// or `sch_netem` is missing. The stack's own transmits go through a `LossyDevice`, whose default policy frees every
// tenth packet and reports it sent. Each case needs at least one resend on the stack's side, at the RTO, of a segment
// marked lost or as a tail probe, and the clean close; the brick cases need timestamps negotiated, which the kernel
// offers by default. Waits are 30 s there. Each case also prints
// how far the kernel's `RetransSegs` (the `Tcp:` rows of `/proc/net/snmp`) rose while it ran: the kernel's own
// resends, for information only, since the counter is system-wide and other traffic moves it too.
//
// `BrickServerProbesAKernelClientThatStopsReading` is the probe against Linux: a kernel client with an 8 KB
// `SO_RCVBUF` sends 50 000 bytes and reads none of the echo until the brick has sent two zero-window probes, so
// the kernel's window closes and the brick probes it, and the kernel's answer to the first probe is seen accepted;
// the connection must stay open, and once the client reads, the echo completes and both sides close cleanly.
//
// The cases of `TcpTapReceive` turn it round: the kernel sends and the brick receives, stepped on the test's thread
// between non-blocking calls on the kernel's socket. They have not yet been run against a kernel, so a first failure
// may be the test's. They check that SACK and window scaling are negotiated whichever
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
// The cases of `TcpTapRecovery` make the stack the sender: it listens, the kernel connects, and the stack's frames go
// through a `LossyDevice` whose policy each case sets, while nothing shapes the kernel's. They check that a tail loss
// is repaired by a tail probe and never by the RTO, that one data segment in fifty lost over 2 MB costs few expiries of
// the RTO against the resends and probes that repair the rest, and that a data segment swapped with the next frame of
// its burst is waited for, not resent, so the kernel sends few duplicate SACKs. The last reads `TCPDSACKOldSent` from
// the `TcpExt:` rows of `/proc/net/netstat`, a system-wide count, so it wants an otherwise idle machine, and skips when
// the field is missing. They step the stack on the test's thread as `TcpTapReceive` does, have not yet been run against
// a kernel either, and are run alone for the same reason:
//
//     sudo ./build/debug/tests/manual_tests/tcp/Aloe.Tests.Manual.Tcp.Tap --gtest_filter='TcpTapRecovery.*'
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

    /// What `LossyDevice` does with one frame the stack hands it.
    enum class LossyAction : std::uint8_t {
        Deliver,         ///< Hands it to the port.
        Drop,            ///< Frees it and reports it sent, as a wire that loses the frame would.
        HoldBehindNext,  ///< Swaps it with the next frame of the same `transmit` call, which goes first; with no next
                         ///< frame in the call, hands it to the port in order.
    };

    /// Decides what `LossyDevice` does with a frame, from its bytes, a whole Ethernet frame, and its index among the
    /// frames the device was given, from zero. It is asked once per frame, in the order the stack gave them.
    using LossyPolicy = std::function<LossyAction(std::span<const std::byte> frame, std::uint64_t index)>;

    /// `LossyDevice`'s default policy: every tenth frame dropped, the rest delivered.
    [[nodiscard]] constexpr LossyAction drop_every_tenth([[maybe_unused]] const std::span<const std::byte> frame,
                                                         const std::uint64_t index) noexcept {
        return (index + 1) % drop_every == 0 ? LossyAction::Drop : LossyAction::Deliver;
    }

    /**
     * @brief The port seen through a device that does with each frame the stack hands it what a `LossyPolicy` says, as
     * a wire that loses frames or swaps neighbours would.
     *
     * A dropped packet is freed, which returns its loan, and counted as accepted, so the stack sees a transmit that
     * succeeded. A held frame swaps places with the next frame of the same call, whose fate the policy then decides:
     * that frame is dropped or handed to the port first, then the held one, so the two leave swapped within the call
     * and nothing outlives it. A hold is not made, and the frame goes in order, when the frame is the last of its call
     * or is itself the next frame of a hold.
     *
     * A frame the port refuses stays with the caller, as does every frame after it. When the port refuses the next
     * frame of a hold, the two are swapped back, so the caller has them as it gave them; when it refuses the held frame
     * after the next one left, the held frame is the first the caller has left. Frames a refusal leaves with the caller
     * were decided already: they are delivered in order, without asking the policy again, when the caller hands them
     * over again, first, as `ShardQueue` does. So no frame is sent twice or lost.
     */
    template <aloe::device::IsDevice Device>
    class LossyDevice {
    public:
        using Packet = typename Device::Packet;

        explicit LossyDevice(Device& owner, LossyPolicy policy = drop_every_tenth)
            : device_{owner},
              policy_{std::move(policy)},
              decided_(owner.queue_count(), 0) {
        }

        [[nodiscard]] std::uint64_t dropped() const noexcept {
            return dropped_;
        }
        /// Frames that left right behind the next frame of their call, swapped with it.
        [[nodiscard]] std::uint64_t held() const noexcept {
            return holds_;
        }
        /// Holds the policy asked for that were not made: the frame was the last of its call or the next frame of a
        /// hold, or the port refused the pair. With `held()`, every hold asked for.
        [[nodiscard]] std::uint64_t holds_not_made() const noexcept {
            return holds_not_made_;
        }
        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return device_.queue_count();
        }
        [[nodiscard]] aloe::wire::MacAddress mac() const noexcept {
            return device_.mac();
        }
        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return device_.mtu();
        }
        [[nodiscard]] bool link_up() const noexcept {
            return device_.link_up();
        }
        [[nodiscard]] const aloe::device::Capabilities& capabilities() const noexcept {
            return device_.capabilities();
        }
        [[nodiscard]] const aloe::device::RssDescription& steering() const noexcept {
            return device_.steering();
        }
        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            aloe::core::force_non_const(this);  // the stack's mutable view of the port, though only the port is written
            return device_.allocate(queue);
        }
        [[nodiscard]] std::size_t receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
            aloe::core::force_non_const(this);  // the stack's mutable view of the port, though only the port is written
            return device_.receive(queue, out);
        }

        /// Hands the packets over one at a time, each as the policy says, a held one right after the next. Returns the
        /// accepted prefix.
        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
            std::uint8_t& decided = decided_[queue];
            std::size_t slot      = 0;
            while (slot < in.size()) {
                LossyAction action = LossyAction::Deliver;
                if (decided > 0) {
                    --decided;  // a frame a refusal left with the caller: in order
                } else {
                    action = decide(in[slot]);
                }
                if (action == LossyAction::HoldBehindNext && slot + 1 < in.size()) {
                    const std::size_t taken = swap_ahead(queue, in.subspan(slot, 2));
                    if (taken < 2) {
                        decided = static_cast<std::uint8_t>(decided + 2 - taken);
                        return slot + taken;
                    }
                    slot += 2;
                    continue;
                }
                if (action == LossyAction::HoldBehindNext) {
                    ++holds_not_made_;  // the last frame of its call: in order
                }
                if (action == LossyAction::Drop) {
                    in[slot] = Packet{};
                    ++dropped_;
                } else if (device_.transmit(queue, in.subspan(slot, 1)) == 0) {
                    ++decided;
                    return slot;
                }
                ++slot;
            }
            return in.size();
        }

        [[nodiscard]] std::size_t reclaim(const std::uint16_t queue) noexcept {
            aloe::core::force_non_const(this);  // the stack's mutable view of the port, though only the port is written
            return device_.reclaim(queue);
        }
        [[nodiscard]] aloe::device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return device_.counters(queue);
        }

    private:
        /// The policy's action for the next frame the stack gave, asked once.
        [[nodiscard]] LossyAction decide(Packet& packet) noexcept {
            const LossyAction action = policy_(packet.data(), asked_);
            ++asked_;
            return action;
        }

        /// `pair[0]`, held, swaps places with `pair[1]`, the next frame of its call, which the policy decides now and
        /// which is dropped or handed over first; then the held frame goes. Returns how many of the two slots were
        /// accepted: none when the port refused the next frame, and the two are swapped back; one when it refused the
        /// held frame, which the caller keeps in the second slot.
        [[nodiscard]] std::size_t swap_ahead(const std::uint16_t queue, std::span<Packet> pair) noexcept {
            std::swap(pair[0], pair[1]);
            const LossyAction action = decide(pair[0]);
            if (action == LossyAction::HoldBehindNext) {
                ++holds_not_made_;  // the next frame of a hold goes as it is
            }
            if (action == LossyAction::Drop) {
                pair[0] = Packet{};
                ++dropped_;
            } else if (device_.transmit(queue, pair.first(1)) == 0) {
                std::swap(pair[0], pair[1]);
                ++holds_not_made_;
                return 0;
            }
            if (device_.transmit(queue, pair.last(1)) == 0) {
                ++holds_not_made_;
                return 1;
            }
            ++holds_;
            return 2;
        }

        Device& device_;
        LossyPolicy policy_;
        /// Per queue, the frames at the front of the caller's next call that the policy decided before a refusal left
        /// them with the caller.
        std::vector<std::uint8_t> decided_;
        std::uint64_t asked_          = 0;  ///< Frames the policy decided: the next one's index.
        std::uint64_t dropped_        = 0;
        std::uint64_t holds_          = 0;
        std::uint64_t holds_not_made_ = 0;
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
            aloe::loop::ShardQueueCounters shard_counters;
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

    /// A counter of one of the kernel's MIB files, `/proc/net/snmp` or `/proc/net/netstat`: its rows come in pairs, a
    /// row of names and a row of values, both opening with `prefix`. Nothing when the file, the rows or the field is
    /// missing. System-wide: every TCP socket of the host moves it.
    [[nodiscard]] std::optional<std::uint64_t>
    kernel_counter(const std::string_view path, const std::string_view prefix, const std::string_view field) {
        std::ifstream mib{std::string{path}};
        std::string names;
        std::string values;
        for (std::string line; std::getline(mib, line);) {
            if (!line.starts_with(prefix)) {
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
            if (name == field) {
                std::uint64_t count     = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
                return error == std::errc{} ? std::optional{count} : std::nullopt;
            }
        }
        return std::nullopt;
    }

    /// The kernel's `RetransSegs`, from the `Tcp:` rows of `/proc/net/snmp`: the TCP segments it sent again.
    [[nodiscard]] std::optional<std::uint64_t> kernel_retrans_segs() {
        return kernel_counter("/proc/net/snmp", "Tcp:", "RetransSegs");
    }

    /// The kernel's `TCPDSACKOldSent`, `nstat`'s `TcpExtTCPDSACKOldSent`, from the `TcpExt:` rows of
    /// `/proc/net/netstat`: the duplicate SACKs it sent for data that arrived again below RCV.NXT.
    [[nodiscard]] std::optional<std::uint64_t> kernel_dsack_old_sent() {
        return kernel_counter("/proc/net/netstat", "TcpExt:", "TCPDSACKOldSent");
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

    /// The stack's resends of every kind: at an expiry of the RTO (`retransmits`), of a segment marked lost
    /// (`fast_retransmits`), and tail probes (`tail_probes`). A loss at the tail of what the stack sent, with nothing
    /// after it that RACK or duplicate ACKs could find it by, is repaired by a probe, which neither of the others
    /// counts.
    [[nodiscard]] std::uint64_t resends(const aloe::tcp::TcpCounters& counters) noexcept {
        return counters.retransmits + counters.fast_retransmits + counters.tail_probes;
    }

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
            EXPECT_GE(resends(loop.counters()), 1U);
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
            EXPECT_GE(resends(loop.counters()), 1U);
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
            EXPECT_GE(resends(counters), 1U);
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
            EXPECT_GE(resends(counters), 1U);
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

    /// Fails with the first offset at which `read`, what `reader` read, and `sent`, what `sender` sent, differ, and
    /// with both sizes when they differ; byte-exact.
    void expect_same_bytes(const std::span<const std::byte> read,
                           const std::span<const std::byte> sent,
                           const std::string_view reader = "the stack",
                           const std::string_view sender = "the kernel") {
        EXPECT_EQ(read.size(), sent.size())
            << "the bytes " << reader << " read against the bytes " << sender << " sent";
        const auto [mine, theirs] = std::ranges::mismatch(read, sent);
        if (mine != read.end() && theirs != sent.end()) {
            ADD_FAILURE() << reader << " read 0x" << std::hex << std::to_integer<unsigned>(*mine) << " where " << sender
                          << " sent 0x" << std::to_integer<unsigned>(*theirs) << std::dec << ", first at offset "
                          << (mine - read.begin()) << " of " << sent.size();
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
        aloe::loop::ShardQueueCounters shard_counters_;
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

    /// Steps `side`, a `ReceiveSide` or a `SendSide`, until `done()` holds or `patience` runs out; true when it held.
    /// `done` runs once per turn, so it is also where a case drives the kernel's socket.
    template <typename Side, typename Done>
    [[nodiscard]] bool step_until(Side& side, Done done) {
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
    template <typename Side>
    [[nodiscard]] int kernel_connect(Side& side, const int send_buffer = 0) {
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

    // The cases of `TcpTapRecovery`: the flights of the tail-loss case, the size of the other two transfers, and the
    // rates of their drops and holds, all the brief's.
    constexpr std::size_t tail_flights    = 200;
    constexpr std::size_t lone_size       = 100;  ///< The tail-loss case's flight of one small segment.
    constexpr std::size_t recovery_size   = std::size_t{2} << 20;
    constexpr std::uint64_t drop_one_in   = 50;
    constexpr std::uint64_t hold_one_in   = 40;
    /// The brief's "a handful" of duplicate SACKs the kernel may send in the reordering case, read as five.
    constexpr std::uint64_t dsack_handful = 5;

    /// The sequence space the payload of a TCP segment covers.
    struct TcpPayload {
        aloe::wire::TcpSequence first{};
        std::size_t length = 0;

        /// The payload carries the byte at `sequence`.
        [[nodiscard]] constexpr bool carries(const aloe::wire::TcpSequence sequence) const noexcept {
            const std::int32_t offset = sequence - first;
            return offset >= 0 && static_cast<std::size_t>(offset) < length;
        }
    };

    /// The payload of the TCP segment `frame` carries, as the stack writes one: IPv4 in Ethernet. Nothing for any other
    /// frame, an ARP one for instance, and for a segment without payload, a pure ACK, a SYN or a FIN alone.
    [[nodiscard]] std::optional<TcpPayload> tcp_payload_of(const std::span<const std::byte> frame) {
        const std::optional<aloe::wire::EthernetHeader> ethernet = aloe::wire::EthernetHeader::parse(frame);
        if (!ethernet || ethernet->ethertype != std::to_underlying(aloe::wire::EtherType::Ipv4)) {
            return std::nullopt;
        }
        const std::span<const std::byte> datagram          = frame.subspan(aloe::wire::EthernetHeader::size);
        const std::optional<aloe::wire::Ipv4Header> header = aloe::wire::Ipv4Header::parse(datagram);
        if (!header || header->protocol != aloe::wire::Ipv4Protocol::Tcp || header->is_fragment()) {
            return std::nullopt;
        }
        const std::span<const std::byte> segment = datagram.subspan(
            header->header_length, static_cast<std::size_t>(header->total_length) - header->header_length);
        const std::optional<aloe::wire::TcpHeader> tcp = aloe::wire::TcpHeader::parse(segment);
        if (!tcp || segment.size() == tcp->data_offset) {
            return std::nullopt;
        }
        return TcpPayload{.first = tcp->sequence, .length = segment.size() - tcp->data_offset};
    }

    /// The send side over the bricks, its frames through a `LossyDevice`, stepped on the test's own thread between the
    /// kernel socket's calls, which never block, for `ReceiveSide`'s reason. The stack listens on `echo_port` and, on
    /// the connection the kernel opens, sends what `offer` gave it as far as the windows and the send cap allow each
    /// turn, the rest at a later one; it reads whatever the kernel sends into `received()`, and closes once `close()`
    /// was asked and everything offered is committed.
    class SendSide {
    public:
        using Device     = LossyDevice<Port>;
        using Tcp        = TcpT<Device>;
        using Connection = Tcp::ConnectionType;

        explicit SendSide(Device& device)
            : queue_{
                  device, 0, 64, shard_counters_
        },
              ip_{queue_, {.address = stack_ip, .prefix = 24}}, wheel_{1ms, aloe::core::Clock::now()},
              tcp_{ip_, wheel_, {}}, burst_(Port::max_burst) {
            EXPECT_TRUE(tcp_.listen(echo_port).has_value()) << "the stack's listen on " << echo_port;
        }

        SendSide(const SendSide&)            = delete;
        SendSide& operator=(const SendSide&) = delete;
        SendSide(SendSide&&)                 = delete;
        SendSide& operator=(SendSide&&)      = delete;
        ~SendSide() {
            queue_.discard();
        }

        /// One turn of the loop: receive, process, fire timers, take events, read, send what is offered, flush.
        void step() {
            const auto now             = aloe::core::Clock::now();
            const std::size_t received = queue_.receive(burst_);
            ip_.process(std::span<Port::Packet>{burst_}.first(received), now);
            tcp_.process(ip_.received(aloe::wire::Ipv4Protocol::Tcp), now);
            std::ignore = wheel_.advance(now);
            while (const auto event = tcp_.poll_event()) {
                auto& c = *event->connection;
                if (event->events.accepted()) {
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
                for (const std::span<const std::byte> chunk : c.unread()) {
                    received_.insert(received_.end(), chunk.begin(), chunk.end());
                }
                c.consume(c.unread().size());
                while (committed_ < offered_.size()) {
                    const std::size_t accepted = c.send(std::span<const std::byte>{offered_}.subspan(committed_));
                    if (accepted == 0) {
                        break;  // the windows or the send cap: the rest at a later turn
                    }
                    committed_ += accepted;
                }
                if (closing_ && !fin_queued_ && committed_ == offered_.size()) {
                    c.close();
                    fin_queued_ = true;
                }
            }
            tcp_.flush(now);
            std::ignore = queue_.flush();
            if (received == 0) {
                std::this_thread::yield();
            }
        }

        /// Appends `bytes` to what the stack sends.
        void offer(const std::span<const std::byte> bytes) {
            offered_.insert(offered_.end(), bytes.begin(), bytes.end());
        }
        /// The stack closes once everything offered is committed.
        void close() noexcept {
            closing_ = true;
        }
        /// The open connection, from its `Accepted` event until its end; null otherwise.
        [[nodiscard]] const Connection* connection() const noexcept {
            return connection_;
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
        aloe::loop::ShardQueueCounters shard_counters_;
        aloe::loop::ShardQueue<Device> queue_;
        Ipv4T<Device> ip_;
        aloe::loop::TimerWheel wheel_;
        Tcp tcp_;
        std::vector<Port::Packet> burst_;
        Connection* connection_ = nullptr;
        std::vector<std::byte> offered_;
        std::size_t committed_ = 0;  ///< The bytes of `offered_` the stack accepted.
        std::vector<std::byte> received_;
        bool closing_     = false;
        bool fin_queued_  = false;
        bool clean_close_ = false;
        bool ended_       = false;
    };

    /// The kernel end against the stack as the sender, pumped once per turn of the stack and never blocking: it reads
    /// what arrived into `read()` and, when it echoes, writes that back as far as the socket's buffer takes, the rest
    /// at a later turn.
    class KernelReader {
    public:
        KernelReader(const int fd, const bool echo)
            : fd_{fd},
              echo_{echo},
              buffer_(std::size_t{1} << 16) {
        }

        /// Reads what arrived, then echoes what is due. False once a call failed with anything but an empty or a full
        /// buffer, which `error()` then names.
        [[nodiscard]] bool pump() {
            while (error_ == 0 && !eof_) {
                const ssize_t got = ::recv(fd_, buffer_.data(), buffer_.size(), MSG_DONTWAIT);
                if (got > 0) {
                    const std::span<const std::byte> chunk =
                        std::span<const std::byte>{buffer_}.first(static_cast<std::size_t>(got));
                    read_.insert(read_.end(), chunk.begin(), chunk.end());
                } else if (got == 0) {
                    eof_ = true;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno != EINTR) {
                    error_ = errno;
                }
            }
            while (echo_ && error_ == 0 && echoed_ < read_.size()) {
                const ssize_t sent =
                    ::send(fd_, read_.data() + echoed_, read_.size() - echoed_, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent >= 0) {
                    echoed_ += static_cast<std::size_t>(sent);
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
        [[nodiscard]] const std::vector<std::byte>& read() const noexcept {
            return read_;
        }
        /// A read returned zero: the stack's FIN arrived after everything it sent.
        [[nodiscard]] bool eof() const noexcept {
            return eof_;
        }
        /// Everything read was written back, or this end does not echo.
        [[nodiscard]] bool echoed_all() const noexcept {
            return !echo_ || echoed_ == read_.size();
        }
        [[nodiscard]] int error() const noexcept {
            return error_;
        }

    private:
        int fd_    = -1;
        bool echo_ = false;
        std::vector<std::byte> buffer_;
        std::vector<std::byte> read_;
        std::size_t echoed_ = 0;
        bool eof_           = false;
        int error_          = 0;
    };

    /// Steps `side` until the kernel has read `size` bytes; false, with the failure reported, when its socket fails,
    /// the stack's connection ends, or `patience` runs out first.
    [[nodiscard]] bool kernel_reads(SendSide& side, KernelReader& kernel, const std::size_t size) {
        const bool done = step_until(
            side, [&side, &kernel, size] { return !kernel.pump() || side.ended() || kernel.read().size() >= size; });
        if (kernel.error() != 0) {
            ADD_FAILURE() << "the kernel's socket failed after " << kernel.read().size()
                          << " bytes: " << std::strerror(kernel.error());
            return false;
        }
        if (!done || kernel.read().size() < size) {
            ADD_FAILURE() << "the kernel read " << kernel.read().size() << " of " << size << " bytes within "
                          << patience().count() << " ms; the stack's connection ended " << side.ended();
            return false;
        }
        return true;
    }

    /// The stack closes once everything offered is committed; the kernel reads to EOF, has echoed everything when it
    /// echoes, and closes its side; the stack's connection ends with both FINs acknowledged and no reset.
    void stack_finishes(SendSide& side, KernelReader& kernel) {
        side.close();
        const bool eof =
            step_until(side, [&kernel] { return !kernel.pump() || (kernel.eof() && kernel.echoed_all()); });
        ASSERT_EQ(kernel.error(), 0) << "the kernel's socket failed: " << std::strerror(kernel.error());
        ASSERT_TRUE(eof) << "the kernel read no EOF within " << patience().count() << " ms, after "
                         << kernel.read().size() << " bytes";
        ASSERT_EQ(shutdown(kernel.fd(), SHUT_WR), 0) << std::strerror(errno);
        EXPECT_TRUE(step_until(side, [&side] { return side.ended(); }))
            << "the stack's connection did not end within " << patience().count() << " ms of the kernel's FIN";
        EXPECT_TRUE(side.clean_close()) << "both FINs acknowledged, no reset";
    }

    /// The stack as the sender against a kernel receiver over the tap: the stack's frames go through a `LossyDevice`
    /// whose policy each case sets, and nothing shapes what the kernel sends. Waits are 30 s, for the 2 MB transfers.
    class TcpTapRecovery : public TcpTap {
    protected:
        void SetUp() override {
            TcpTap::SetUp();
            if (IsSkipped() || HasFatalFailure()) {
                return;
            }
            patience_storage().store(lossy_patience);
        }

        void TearDown() override {
            patience_storage().store(default_patience);
        }
    };

    /// The stack's counters of its loss recovery, printed and recorded as properties for the run's record.
    void report_send_counts(const aloe::tcp::TcpCounters& stack) {
        std::println("the stack: retransmits {}, fast_retransmits {}, recoveries {}, tail_probes {} (needless {}, "
                     "repaired {}, early {}), rack_marked {}, lost_retransmits {}, reorder_timeouts {}, "
                     "reordering_seen {}, duplicate_sacks_received {}, undos_duplicate_sack {}, undos_original_ack {}",
                     stack.retransmits,
                     stack.fast_retransmits,
                     stack.recoveries,
                     stack.tail_probes,
                     stack.tail_probes_needless,
                     stack.tail_probe_recoveries,
                     stack.tail_probes_early,
                     stack.rack_marked,
                     stack.lost_retransmits,
                     stack.reorder_timeouts,
                     stack.reordering_seen,
                     stack.duplicate_sacks_received,
                     stack.undos_duplicate_sack,
                     stack.undos_original_ack);
        ::testing::Test::RecordProperty("stack_retransmits", std::to_string(stack.retransmits));
        ::testing::Test::RecordProperty("stack_fast_retransmits", std::to_string(stack.fast_retransmits));
        ::testing::Test::RecordProperty("stack_tail_probes", std::to_string(stack.tail_probes));
        ::testing::Test::RecordProperty("stack_reordering_seen", std::to_string(stack.reordering_seen));
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

TEST_F(TcpTapRecovery, EveryTailLossIsRepairedByAProbe) {
    // 200 flights, alternately one segment of 100 bytes and three of mss(), each sent once the kernel's echo of the one
    // before is back. Before each flight the case hands the policy the sequence of the flight's last byte, and the
    // policy drops the first transmission of the segment that carries it: a loss at the tail every time, with nothing
    // sent after it for RACK or duplicate ACKs to find it by. The kernel acknowledges and echoes what arrived, the
    // first two segments of a three, then sends nothing. The stack's tail probe leaves two smoothed round trips after
    // the last ACK, plus 40 ms with one segment in flight, ahead of the RTO's 200 ms or more (`rto_min`), and sends the
    // lost segment again; the kernel acknowledges and echoes it. The ACK of a probe lands on the probe's end, which
    // leaves its verdict open, and a lone segment lost next is probed all the same once a round-trip sample was taken
    // since the probe: the probe's ACK gives one from its echo, with timestamps, which the kernel offers by default.
    //
    // Hence every byte echoed, one drop per flight, at least one probe per flight (more when one leaves before the
    // kernel's ACK of what did arrive, two round trips being short on the tap), and no expiry of the RTO.
    std::optional<aloe::wire::TcpSequence> last_byte;
    LossyDevice<Port> device{
        *port_, [&last_byte](const std::span<const std::byte> frame, [[maybe_unused]] const std::uint64_t index) {
            const std::optional<TcpPayload> payload = tcp_payload_of(frame);
            if (!last_byte || !payload || !payload->carries(*last_byte)) {
                return LossyAction::Deliver;
            }
            last_byte.reset();
            return LossyAction::Drop;
        }};
    SendSide side{device};
    const KernelSocket kernel{kernel_connect(side)};
    ASSERT_GE(kernel.fd(), 0);
    // The kernel's echo of a short segment never waits for the ACK of the one before (Nagle).
    const int no_delay = 1;
    ASSERT_EQ(setsockopt(kernel.fd(), IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)), 0)
        << std::strerror(errno);
    ASSERT_TRUE(step_until(side, [&side] { return side.connection() != nullptr; }))
        << "the stack raised no Accepted within " << patience().count() << " ms";
    const auto& c = *side.connection();
    ASSERT_TRUE(c.sack()) << "the kernel offers SACK-permitted by default";
    ASSERT_TRUE(c.timestamps()) << "the kernel offers timestamps by default";
    const std::size_t three = 3U * c.mss();
    const auto stream       = numbered_words(tail_flights / 2 * (lone_size + three));
    KernelReader echo{kernel.fd(), true};
    std::size_t offered = 0;
    for (std::size_t flight = 0; flight < tail_flights; ++flight) {
        const std::size_t size = flight % 2 == 0 ? lone_size : three;
        last_byte              = c.committed() + static_cast<std::uint32_t>(size - 1);
        side.offer(std::span<const std::byte>{stream}.subspan(offered, size));
        offered           += size;
        const bool echoed  = step_until(side, [&side, &echo, offered] {
            return !echo.pump() || side.ended() || side.received().size() >= offered;
        });
        ASSERT_EQ(echo.error(), 0) << "flight " << flight
                                   << ": the kernel's socket failed: " << std::strerror(echo.error());
        ASSERT_FALSE(side.ended()) << "flight " << flight << ": the stack's connection ended";
        ASSERT_TRUE(echoed) << "flight " << flight << ": " << side.received().size() << " of " << offered
                            << " bytes echoed within " << patience().count() << " ms";
    }
    stack_finishes(side, echo);
    expect_same_bytes(echo.read(), stream, "the kernel", "the stack");
    expect_same_bytes(side.received(), stream, "the stack", "it");
    const aloe::tcp::TcpCounters& counters = side.counters();
    report_send_counts(counters);
    EXPECT_EQ(device.dropped(), tail_flights) << "the first transmission of each flight's last segment";
    EXPECT_GE(counters.tail_probes, tail_flights) << "a probe repaired each flight's tail";
    EXPECT_EQ(counters.retransmits, 0U) << "no expiry of the RTO";
}

TEST_F(TcpTapRecovery, ScatteredLossRarelyNeedsTheRto) {
    // 2 MB from the stack, with every fiftieth frame that carries data dropped, a resend or a probe included, as a wire
    // that loses 2% would. A loss with later segments behind it is found by RACK from the kernel's SACK blocks and
    // resent as a segment marked lost (`fast_retransmits`); a loss at a tail is repaired by a probe (`tail_probes`);
    // only a lost probe, or a lost resend with nothing sent after it, waits for the RTO (`retransmits`, one per
    // expiry). The spec's "RTO expiries a small share of the losses" is the brief's tenth: ten times `retransmits` is
    // at most `fast_retransmits + tail_probes`. The payload takes at least `size / mss()` frames of data, so at least a
    // fiftieth of that many are dropped.
    LossyDevice<Port> device{*port_,
                             [segments = std::uint64_t{0}](const std::span<const std::byte> frame,
                                                           [[maybe_unused]] const std::uint64_t index) mutable {
                                 if (!tcp_payload_of(frame)) {
                                     return LossyAction::Deliver;
                                 }
                                 return ++segments % drop_one_in == 0 ? LossyAction::Drop : LossyAction::Deliver;
                             }};
    SendSide side{device};
    const KernelSocket kernel{kernel_connect(side)};
    ASSERT_GE(kernel.fd(), 0);
    ASSERT_TRUE(step_until(side, [&side] { return side.connection() != nullptr; }))
        << "the stack raised no Accepted within " << patience().count() << " ms";
    ASSERT_TRUE(side.connection()->sack()) << "the kernel offers SACK-permitted by default";
    const std::size_t mss = side.connection()->mss();
    const auto payload    = numbered_words(recovery_size);
    KernelReader reader{kernel.fd(), false};
    side.offer(payload);
    ASSERT_TRUE(kernel_reads(side, reader, payload.size()));
    stack_finishes(side, reader);
    expect_same_bytes(reader.read(), payload, "the kernel", "the stack");
    const aloe::tcp::TcpCounters& counters = side.counters();
    report_send_counts(counters);
    EXPECT_GE(device.dropped(), payload.size() / mss / drop_one_in);
    EXPECT_LE(10U * counters.retransmits, counters.fast_retransmits + counters.tail_probes)
        << "the RTO expired " << counters.retransmits << " times, more than a tenth of the "
        << counters.fast_retransmits << " resends of segments marked lost and " << counters.tail_probes
        << " tail probes, over " << device.dropped() << " drops";
}

TEST_F(TcpTapRecovery, ReorderingWithinABurstIsSeenAndNotResent) {
    // 2 MB from the stack, with every fortieth frame that carries data swapped with the next frame of its `transmit`
    // call, as a wire reorders neighbours; one that is the last of its call goes in order, a hold not made. The kernel
    // receives the next frame first, normally the next segment, above a gap, answers it at once with a SACK block, then
    // receives the held segment, which fills the gap, and acknowledges both. The two were sent in one turn of the
    // stack, so they carry one stamp, and once the next is reported RACK waits out its whole reordering window before
    // it would mark the held one lost: the window a connection starts with, a quarter of the minimum round trip.
    // Samples taken from send stamps keep that minimum at tens of microseconds on the tap, above zero, and one taken
    // from a timestamp echo counts at least a millisecond, as Linux's `tcp_rtt_tsopt_us` counts it, so it never pulls
    // the minimum down. The cumulative ACK comes within the window and fills a hole below the highest sequence
    // reported with a segment sent once: reordering seen, nothing resent. Hence the kernel's duplicate SACKs rise by
    // at most a handful (a reordering timer that fires before a cumulative ACK the stack reads a turn after its SACK;
    // a needless probe at the end of the transfer), and the stack saw reordering.
    //
    // A swap within a burst needs nothing learned: the window's growth from duplicate reports is pinned by the
    // fabric's two-stack case, `AFrameHeldBehindFourIsResentOnceAndNotTheSecondTime`, not here. Every hold asked for
    // is made or not, and at least `size / mss() / 40` were asked, the payload taking at least `size / mss()` frames
    // of data. At least one must be made for the case to pin anything; how many are depends on how the stack's frames
    // fall into calls, so the case prints both counts beside the kernel's.
    const std::optional<std::uint64_t> dsacks_before = kernel_dsack_old_sent();
    if (!dsacks_before) {
        GTEST_SKIP() << "/proc/net/netstat has no TCPDSACKOldSent in its TcpExt rows";
    }
    LossyDevice<Port> device{*port_,
                             [segments = std::uint64_t{0}](const std::span<const std::byte> frame,
                                                           [[maybe_unused]] const std::uint64_t index) mutable {
                                 if (!tcp_payload_of(frame)) {
                                     return LossyAction::Deliver;
                                 }
                                 return ++segments % hold_one_in == 0 ? LossyAction::HoldBehindNext
                                                                      : LossyAction::Deliver;
                             }};
    SendSide side{device};
    const KernelSocket kernel{kernel_connect(side)};
    ASSERT_GE(kernel.fd(), 0);
    ASSERT_TRUE(step_until(side, [&side] { return side.connection() != nullptr; }))
        << "the stack raised no Accepted within " << patience().count() << " ms";
    const auto& c = *side.connection();
    ASSERT_TRUE(c.sack()) << "the kernel offers SACK-permitted by default";
    const std::size_t mss = c.mss();
    const auto payload    = numbered_words(recovery_size);
    KernelReader reader{kernel.fd(), false};
    side.offer(payload);
    ASSERT_TRUE(kernel_reads(side, reader, payload.size()));
    // What RACK stood on at the end of the transfer, for the record: the window and the smoothed round trip it is
    // capped by.
    const auto window = std::chrono::duration_cast<std::chrono::microseconds>(c.reordering_window());
    const auto srtt =
        std::chrono::duration_cast<std::chrono::microseconds>(c.srtt().value_or(aloe::core::Duration::zero()));
    stack_finishes(side, reader);
    const std::optional<std::uint64_t> dsacks_after = kernel_dsack_old_sent();
    ASSERT_TRUE(dsacks_after.has_value()) << "/proc/net/netstat lost its TCPDSACKOldSent";
    const std::uint64_t dsacks = *dsacks_after - *dsacks_before;
    expect_same_bytes(reader.read(), payload, "the kernel", "the stack");
    const aloe::tcp::TcpCounters& counters = side.counters();
    report_send_counts(counters);
    std::println("the device swapped {} frames with the next of their call, and left in order {} it was asked to hold "
                 "(the last of their call, or refused by the port); the kernel's TCPDSACKOldSent rose by {} "
                 "(system-wide); at the end of the transfer the stack's reordering window was {} us and its smoothed "
                 "round trip {} us",
                 device.held(),
                 device.holds_not_made(),
                 dsacks,
                 window.count(),
                 srtt.count());
    RecordProperty("device_held", std::to_string(device.held()));
    RecordProperty("device_holds_not_made", std::to_string(device.holds_not_made()));
    RecordProperty("kernel_dsack_old_sent", std::to_string(dsacks));
    EXPECT_GE(device.held() + device.holds_not_made(), payload.size() / mss / hold_one_in)
        << "every hold asked for is made or not, and the payload's frames of data ask for at least this many";
    ASSERT_GT(device.held(), 0U) << "no frame the policy held had a next frame in its call: the device swapped "
                                    "nothing, and the case pins nothing about the stack";
    EXPECT_LE(dsacks, dsack_handful) << "the kernel's duplicate SACKs over " << device.held()
                                     << " swaps within a burst: the stack resent segments its starting reordering "
                                        "window covers";
    EXPECT_GT(counters.reordering_seen, 0U)
        << "no ACK showed the stack the reordering of " << device.held() << " swaps";
}
