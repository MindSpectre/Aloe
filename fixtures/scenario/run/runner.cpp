#include "runner.hpp"

#include <aloe/core>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <harness.hpp>
#include <match.hpp>
#include <notation.hpp>
#include <peer.hpp>
#include <script.hpp>

namespace aloe::scenario {

    namespace {

        /// An inbound line's window when none is written.
        constexpr std::uint16_t default_window = 65535;

        /// TCP's header length counts 32-bit words, so an option list fills whole words.
        constexpr std::size_t option_alignment = 4;

        /// A stamp in the script's notation: seconds since the epoch.
        [[nodiscard]] std::string at(const core::TimePoint stamp) {
            return format_time(stamp - core::TimePoint{});
        }

        /// A window as a script reads it: one stamp when it is a point, else both ends.
        [[nodiscard]] std::string span_of(const core::TimePoint low, const core::TimePoint high) {
            return low == high ? at(low) : std::format("[{}..{}]", at(low), at(high));
        }

        [[nodiscard]] std::string_view name_of(const tcp::ListenError error) noexcept {
            switch (error) {
                case tcp::ListenError::InUse:
                    return "InUse";
                case tcp::ListenError::TableFull:
                    return "TableFull";
            }
            std::unreachable();
        }

    }  // namespace

    Runner::Runner(Harness& harness, const Script& script, const RunnerConfig& config)
        : harness_{&harness},
          script_{&script},
          config_{config},
          peer_{harness.config().peer_isn, harness.config().peer_port, harness.config().stack_port} {
        if (script.tolerance) {
            config_.tolerance = *script.tolerance;
        }
    }

    std::expected<void, Failure> Runner::run() {
        for (const Event& event : script_->events) {
            if (auto done = run_event(event); !done) {
                return done;
            }
        }
        return check_end();
    }

    Runner::Outcome Runner::run_event(const Event& event) {
        if (const auto* call = std::get_if<CallLine>(&event.action)) {
            return run_call(event, *call);
        }
        const auto& line = std::get<PacketLine>(event.action);
        return line.direction == Direction::Inbound ? run_inbound(event, line) : run_outbound(event, line);
    }

    Runner::Outcome Runner::run_inbound(const Event& event, const PacketLine& line) {
        if (line.options.any) {
            return fail(event, "inbound options must be written");
        }
        if (line.urgent) {
            return fail(event, "inbound urg is not supported");  // frames::TcpSpec carries no urgent pointer
        }
        if (auto reached = reach(event); !reached) {
            return reached;
        }
        auto options = encode_options(line.options, peer_.inbound_sack_base());  // the base the steps may have fixed
        if (options.size() % option_alignment != 0) {
            return fail(event, "inbound options must be a multiple of 4 bytes; pad with nop");
        }
        const HarnessConfig& config = harness_->config();
        const auto frame =
            frames::tcp_frame({.destination_mac  = config.stack_mac,
                               .source_mac       = config.peer_mac,
                               .source           = config.peer_address,
                               .destination      = config.stack_address,
                               .source_port      = peer_.peer_port(),
                               .destination_port = peer_.stack_port(),
                               .sequence         = peer_.sequence(line.first),
                               .acknowledgement  = line.ack ? peer_.acknowledgement(*line.ack) : wire::TcpSequence{},
                               .flags            = line.flags,
                               .window           = line.window.value_or(default_window),
                               .options          = std::move(options)},
                              frames::pattern(line.length, static_cast<std::uint8_t>(line.first)));
        harness_->inject(frame, now_);
        return settle(event);
    }

    Runner::Outcome Runner::run_outbound(const Event& event, const PacketLine& line) {
        const Window window        = window_of(event);
        const core::TimePoint last = window.high + config_.tolerance;
        while (queue_.empty() && now_ <= last) {
            if (auto stepped = step(event); !stepped) {
                return stepped;
            }
        }
        if (!queue_.empty()) {
            return take_head(event, line, window);
        }
        const std::string expected = describe(line, peer_.outbound_sack_base());
        if (event.time.kind == TimeSpec::Kind::Any) {
            return fail(event,
                        std::format("missing: expected \"{}\" within the bound of {}",
                                    expected,
                                    format_time(config_.any_bound)));
        }
        return fail(
            event,
            std::format("missing: expected \"{}\" in window [{}..{}]", expected, at(window.low), at(window.high)));
    }

    Runner::Outcome Runner::run_call(const Event& event, const CallLine& call) {
        if (auto reached = reach(event); !reached) {
            return reached;
        }
        if (auto called = invoke(event, call); !called) {
            return called;
        }
        return settle(event);
    }

    Runner::Outcome Runner::check_end() const {
        if (queue_.empty()) {
            return {};
        }
        // The queue fills only while an event runs, so there is a last event to name.
        const Seen& head = queue_.front();
        return fail(script_->events.back(),
                    std::format("unexpected: the stack sent \"{}\" at {} after the script's end",
                                describe(head.segment, peer_.outbound_sack_base()),
                                at(head.stamp)));
    }

    Runner::Outcome Runner::reach(const Event& event) {
        const core::TimePoint low = window_of(event).low;
        if (event.time.kind == TimeSpec::Kind::Absolute && low < now_) {
            return fail(event, std::format("time goes backwards: {} is before {}", at(low), at(now_)));
        }
        while (now_ < low) {
            if (auto stepped = step(event); !stepped) {
                return stepped;
            }
        }
        return {};
    }

    Runner::Outcome Runner::settle(const Event& event) {
        drain_events();
        harness_->flush(now_);
        if (auto collected = collect(event); !collected) {
            return collected;
        }
        previous_ = now_;
        return {};
    }

    Runner::Outcome Runner::take_head(const Event& event, const PacketLine& line, const Window window) {
        const Seen& head             = queue_.front();
        const wire::TcpSequence base = peer_.outbound_sack_base();
        const std::string seen =
            std::format("the stack sent \"{}\" at {}", describe(head.segment, base), at(head.stamp));
        if (!head.checksums_ok) {
            return fail(event, "bad checksum: " + seen);
        }
        const std::string expected =
            std::format("expected \"{}\" at {}; {}", describe(line, base), span_of(window.low, window.high), seen);
        if (const auto reason = match(line, head.segment, base)) {
            return fail(event, std::format("{}: {}", expected, *reason));
        }
        if (head.stamp < window.low - config_.tolerance) {
            return fail(event, "early: " + expected);
        }
        if (head.stamp > window.high + config_.tolerance) {
            return fail(event, "late: " + expected);
        }
        previous_ = head.stamp;
        queue_.pop_front();
        return {};
    }

    Runner::Outcome Runner::step(const Event& event) {
        now_ += harness_->config().resolution;
        harness_->step(now_);
        drain_events();
        harness_->flush(now_);
        return collect(event);
    }

    Runner::Outcome Runner::collect(const Event& event) {
        for (const Collected& item : harness_->collect()) {
            auto segment = peer_.observe(item.frame);
            if (!segment) {
                return fail(event, "unexpected non-TCP frame");
            }
            queue_.push_back({.segment      = std::move(*segment),
                              .stamp        = now_,
                              .checksums_ok = item.ipv4_checksum_ok && item.tcp_checksum_ok});
        }
        return {};
    }

    void Runner::drain_events() {
        while (harness_->tcp().poll_event()) {}
    }

    Runner::Window Runner::window_of(const Event& event) const {
        const TimeSpec& time = event.time;
        switch (time.kind) {
            case TimeSpec::Kind::Relative:
                return {.low = previous_ + time.low, .high = previous_ + time.high};
            case TimeSpec::Kind::Absolute:
                return {.low = core::TimePoint{} + time.low, .high = core::TimePoint{} + time.high};
            case TimeSpec::Kind::Any:  // from the start, so a segment already waiting in the queue is never early
                return {.low = core::TimePoint{}, .high = now_ + config_.any_bound};
        }
        std::unreachable();
    }

    Runner::Outcome Runner::invoke(const Event& event, const CallLine& call) {
        using Bound                 = Outcome (Runner::*)(const Event&, const CallLine&);
        static constexpr auto calls = std::to_array<std::pair<std::string_view, Bound>>({
            {"socket",     &Runner::call_socket    },
            {"setsockopt", &Runner::call_setsockopt},
            {"fcntl",      &Runner::call_fcntl     },
            {"bind",       &Runner::call_bind      },
            {"listen",     &Runner::call_listen    },
        });
        for (const auto& [name, bound] : calls) {
            if (name == call.name) {
                return std::invoke(bound, this, event, call);
            }
        }
        return fail(event, std::format("unbound call {}", call.name));
    }

    Runner::Outcome Runner::call_socket(const Event& event, const CallLine& call) {
        if (!descriptors_.try_emplace(call.result).second) {
            return fail(event, std::format("descriptor {} is in use", call.result));
        }
        return {};
    }

    Runner::Outcome Runner::call_setsockopt(const Event& event, const CallLine& call) {
        if (auto found = descriptor(event, call); !found) {
            return std::unexpected{std::move(found.error())};
        }
        // Accepted and ignored: Aloe reuses ports and has no Nagle.
        const std::string_view option = call.arguments.size() > 2 ? call.arguments[2].text : std::string_view{};
        if (option != "SO_REUSEADDR" && option != "TCP_NODELAY") {
            return fail(event, std::format("unbound call setsockopt({})", option));
        }
        return expect_zero(event, call);
    }

    Runner::Outcome Runner::call_fcntl(const Event& event, const CallLine& call) {
        // Accepted and ignored: every verb is non-blocking.
        if (auto found = descriptor(event, call); !found) {
            return std::unexpected{std::move(found.error())};
        }
        return expect_zero(event, call);
    }

    Runner::Outcome Runner::call_bind(const Event& event, const CallLine& call) {
        auto found = descriptor_of(event, call, Binding::Kind::Socket);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        if (auto zero = expect_zero(event, call); !zero) {
            return zero;
        }
        (*found)->port = harness_->config().stack_port;
        return {};
    }

    Runner::Outcome Runner::call_listen(const Event& event, const CallLine& call) {
        auto found = descriptor_of(event, call, Binding::Kind::Socket);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        Binding& binding = **found;
        if (binding.port == 0) {
            return fail(event, std::format("listen: descriptor {} is not bound", call.arguments.front().integer));
        }
        if (auto zero = expect_zero(event, call); !zero) {
            return zero;
        }
        if (const auto listened = harness_->tcp().listen(binding.port); !listened) {
            return fail(event, std::format("listen: {}", name_of(listened.error())));
        }
        binding.kind = Binding::Kind::Listener;
        return {};
    }

    std::expected<Runner::Binding*, Failure> Runner::descriptor(const Event& event, const CallLine& call) {
        if (call.arguments.empty() || call.arguments.front().kind != Argument::Kind::Integer) {
            return fail(event, std::format("{}: the first argument is not a descriptor", call.name));
        }
        const std::int64_t number = call.arguments.front().integer;
        const auto found          = descriptors_.find(number);
        if (found == descriptors_.end()) {
            return fail(event, std::format("{}: descriptor {} is not open", call.name, number));
        }
        return &found->second;
    }

    std::expected<Runner::Binding*, Failure>
    Runner::descriptor_of(const Event& event, const CallLine& call, const Binding::Kind kind) {
        auto found = descriptor(event, call);
        if (found && (*found)->kind != kind) {
            return fail(
                event,
                std::format(
                    "{}: descriptor {} is not a {}", call.name, call.arguments.front().integer, kind_name(kind)));
        }
        return found;
    }

    std::string_view Runner::kind_name(const Binding::Kind kind) noexcept {
        switch (kind) {
            case Binding::Kind::Socket:
                return "socket";
            case Binding::Kind::Listener:
                return "listener";
        }
        std::unreachable();
    }

    std::unexpected<Failure> Runner::fail(const Event& event, std::string message) {
        Failure failure{.line = event.line, .message = std::move(message)};
        return std::unexpected{std::move(failure)};
    }

    Runner::Outcome Runner::expect_zero(const Event& event, const CallLine& call) {
        if (call.result != 0) {
            return fail(event, std::format("{}: result {} is not 0", call.name, call.result));
        }
        return {};
    }

    std::string format(const Failure& failure, const std::string_view script_path) {
        return std::format("{}:{}: {}", script_path, failure.line, failure.message);
    }

}  // namespace aloe::scenario
