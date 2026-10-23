#pragma once

#include <aloe/core>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
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
     * check its stamp against the line's window widened by the tolerance on both sides.
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
            enum class Kind : std::uint8_t { Socket, Listener };
            Kind kind          = Kind::Socket;
            std::uint16_t port = 0;  ///< Set by `bind`; zero until then.
        };

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
        void drain_events();
        [[nodiscard]] Window window_of(const Event& event) const;

        [[nodiscard]] Outcome invoke(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_socket(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_setsockopt(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_fcntl(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_bind(const Event& event, const CallLine& call);
        [[nodiscard]] Outcome call_listen(const Event& event, const CallLine& call);

        /// The binding the call's first argument names; it fails when the descriptor is not open.
        [[nodiscard]] std::expected<Binding*, Failure> descriptor(const Event& event, const CallLine& call);

        /// As `descriptor`, and the binding must be of `kind`.
        [[nodiscard]] std::expected<Binding*, Failure>
        descriptor_of(const Event& event, const CallLine& call, Binding::Kind kind);

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
    };

    /// "<path>:<line>: <message>" for a report.
    [[nodiscard]] std::string format(const Failure& failure, std::string_view script_path);

}  // namespace aloe::scenario
