#pragma once

#include <aloe/core>
#include <aloe/stream>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <harness.hpp>
#include <peer.hpp>
#include <script.hpp>

namespace aloe::scenario {

    struct RunnerConfig {
        /// Widens outbound windows both ways; the script's --tolerance_usecs overrides.
        core::Duration tolerance = std::chrono::milliseconds{1};
        core::Duration any_bound = std::chrono::seconds{120};  ///< How long a `*` line waits before it is missing.
    };

    /// Where a script failed: the event's 1-based source line and what was expected, found and when.
    struct Failure {
        std::size_t line = 0;
        std::string message{};
    };

    /**
     * @brief Runs a script over a harness on a virtual clock, one wheel resolution per step.
     *
     * Inbound lines and calls act at their nominal time, with no tolerance. Outbound lines take the head of the queue
     * of segments the stack sent, in order, across the inbound lines and calls in between, as packetdrill does, and
     * check its stamp against the line's window widened by the tolerance on both sides. An `accept`, `read` or `recv`
     * written with a window waits for its expected result, one step at a time, until the window closes.
     */
    class Runner {
    public:
        Runner(Harness& harness, const Script& script, const RunnerConfig& config = {});

        /// Runs the script to its end, or to the first failure. Call once.
        [[nodiscard]] std::expected<void, Failure> run();

    private:
        using Outcome = std::expected<void, Failure>;

        /// A segment the stack sent, with the step it was collected at.
        struct Seen {
            ObservedSegment segment{};
            core::TimePoint stamp{};
            bool checksums_ok = false;
        };

        /// What a script descriptor stands for.
        struct Binding {
            enum class Kind : std::uint8_t { Socket, Listener, Connection, Closed };
            Kind kind                       = Kind::Socket;
            std::uint16_t port              = 0;  ///< Set by `bind`; zero until then.
            /// A connection's slot, or a closed one's until its terminal event; null once released.
            Harness::Connection* connection = nullptr;
            bool established                = false;  ///< `Connected` or `Accepted` was seen.
            std::optional<stream::Event> terminal{};  ///< `Closed`, `Reset`, `TimedOut` or `Unresolved`, once seen.
        };

        /// One try of a call that waits: true when its expected result holds, false while it does not yet.
        using Attempt = std::expected<bool, Failure>;
        using Try     = Attempt (Runner::*)(const Event&, const CallLine&);

        /// The stamps an event may happen between, both inclusive.
        struct Window {
            core::TimePoint low{};
            core::TimePoint high{};
        };

        [[nodiscard]] Outcome run_event(const Event& event);
        [[nodiscard]] Outcome run_inbound(const Event& event, const PacketLine& line);
        [[nodiscard]] Outcome run_outbound(const Event& event, const PacketLine& line);
        [[nodiscard]] Outcome run_call(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome check_end() const;

        /// Steps up to the event's nominal time; an absolute time before `now_` fails.
        [[nodiscard]] Outcome reach(const Event& event);

        /// After an inbound line or a call: the events, the flush and the collect at the same stamp.
        [[nodiscard]] Outcome settle(const Event& event);

        /// Checks the queue's head against an outbound line and its window, and pops it.
        [[nodiscard]] Outcome take_head(const Event& event, const PacketLine& line, Window window);

        [[nodiscard]] Outcome step(const Event& event);
        [[nodiscard]] Outcome collect(const Event& event);

        /// Binds the stack's events to the descriptors; a terminal event releases its connection.
        void drain_events();
        void on_event(const Harness::Event& polled);
        [[nodiscard]] Window window_of(const Event& event) const;

        [[nodiscard]] Outcome invoke(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_socket(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_setsockopt(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_fcntl(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_bind(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_listen(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_accept(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_connect(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_write(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_read(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_close(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_shutdown(const Event& event, const CallLine& call);

        /// Tries `attempt` and, while it is unsatisfied and the event has a window, steps and tries again until the
        /// window's end; still unsatisfied, it fails with what was found.
        [[nodiscard]] Outcome wait(const Event& event, const CallLine& call, Try attempt);
        [[nodiscard]] Attempt try_accept(const Event& event, const CallLine& call);
        [[nodiscard]] Attempt try_read(const Event& event, const CallLine& call);

        /// Whether a `read` or `recv` of result `-1` finds the errno word the line expects.
        [[nodiscard]] static Attempt errno_holds(const Event& event, const CallLine& call, const Binding& binding);

        /// What an unsatisfied wait found at the call's descriptor, for its failure.
        [[nodiscard]] std::string found_at(const CallLine& call) const;

        /// Binds `number` as a new descriptor; a negative number or one in use fails.
        [[nodiscard]] Outcome
        open_descriptor(const Event& event, const CallLine& call, std::int64_t number, Binding binding);

        /// The binding the call's first argument names; it fails when the descriptor is not open.
        [[nodiscard]] std::expected<Binding*, Failure> descriptor(const Event& event, const CallLine& call);

        /// As `descriptor`, and the binding must be of `kind`.
        [[nodiscard]] std::expected<Binding*, Failure>
        descriptor_of(const Event& event, const CallLine& call, Binding::Kind kind);

        /// As `descriptor_of` a connection, and its slot must not be released yet.
        [[nodiscard]] std::expected<Binding*, Failure> live_connection(const Event& event, const CallLine& call);

        /// The third argument of a `write`, `send`, `read` or `recv`: the count of bytes.
        [[nodiscard]] static std::expected<std::size_t, Failure> count_of(const Event& event, const CallLine& call);

        /// A `send` or `recv` whose flags, the fourth argument, are not 0 fails.
        [[nodiscard]] static Outcome expect_no_flags(const Event& event, const CallLine& call);

        [[nodiscard]] static std::string_view kind_name(Binding::Kind kind) noexcept;

        [[nodiscard]] static std::unexpected<Failure> fail(const Event& event, std::string message);

        /// A call whose expected result is not zero fails: `<name>: result <r> is not 0`.
        [[nodiscard]] static Outcome expect_zero(const Event& event, const CallLine& call);

        Harness* harness_     = nullptr;
        const Script* script_ = nullptr;
        RunnerConfig config_{};
        Peer peer_;
        core::TimePoint now_{};
        core::TimePoint previous_{};  ///< The previous event's actual time.
        std::deque<Seen> queue_{};
        std::map<std::int64_t, Binding> descriptors_{};
        std::deque<Harness::Connection*> pending_accepts_{};  ///< Accepted, not yet taken by `accept`; oldest first.
    };

    /// "<path>:<line>: <message>" for a report.
    [[nodiscard]] std::string format(const Failure& failure, std::string_view script_path);

}  // namespace aloe::scenario
