#include "runner.hpp"

#include <algorithm>
#include <aloe/core>
#include <aloe/frames>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
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

        [[nodiscard]] std::string_view name_of(const tcp::ConnectError error) noexcept {
            switch (error) {
                case tcp::ConnectError::TableFull:
                    return "TableFull";
                case tcp::ConnectError::NoPort:
                    return "NoPort";
                case tcp::ConnectError::Unplaceable:
                    return "Unplaceable";
                case tcp::ConnectError::NoRoute:
                    return "NoRoute";
            }
            std::unreachable();
        }

        [[nodiscard]] std::string_view name_of(const tcp::State state) noexcept {
            switch (state) {
                case tcp::State::Closed:
                    return "Closed";
                case tcp::State::SynSent:
                    return "SynSent";
                case tcp::State::SynReceived:
                    return "SynReceived";
                case tcp::State::Established:
                    return "Established";
                case tcp::State::FinWait1:
                    return "FinWait1";
                case tcp::State::FinWait2:
                    return "FinWait2";
                case tcp::State::CloseWait:
                    return "CloseWait";
                case tcp::State::Closing:
                    return "Closing";
                case tcp::State::LastAck:
                    return "LastAck";
            }
            std::unreachable();
        }

        [[nodiscard]] std::string_view name_of(const stream::Event event) noexcept {
            switch (event) {
                case stream::Event::Connected:
                    return "Connected";
                case stream::Event::Accepted:
                    return "Accepted";
                case stream::Event::Readable:
                    return "Readable";
                case stream::Event::Writable:
                    return "Writable";
                case stream::Event::Acked:
                    return "Acked";
                case stream::Event::PeerClosed:
                    return "PeerClosed";
                case stream::Event::Closed:
                    return "Closed";
                case stream::Event::Reset:
                    return "Reset";
                case stream::Event::TimedOut:
                    return "TimedOut";
                case stream::Event::Unresolved:
                    return "Unresolved";
            }
            std::unreachable();
        }

        /// The events that end a connection; the stack raises one of them, once.
        constexpr std::array terminal_events{
            stream::Event::Closed, stream::Event::Reset, stream::Event::TimedOut, stream::Event::Unresolved};

        [[nodiscard]] std::optional<stream::Event> terminal_of(const stream::Events events) noexcept {
            for (const stream::Event event : terminal_events) {
                if (events.has(event)) {
                    return event;
                }
            }
            return std::nullopt;
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
        while (const auto polled = harness_->tcp().poll_event()) {
            on_event(*polled);
        }
    }

    void Runner::on_event(const Harness::Event& polled) {
        Harness::Connection* connection             = polled.connection;
        const stream::Events events                 = polled.events;
        const std::optional<stream::Event> terminal = terminal_of(events);
        const auto bound                            = std::ranges::find_if(
            descriptors_, [connection](const auto& entry) { return entry.second.connection == connection; });
        if (bound != descriptors_.end()) {
            Binding& binding    = bound->second;
            binding.established = binding.established || events.connected() || events.accepted();
            if (terminal) {
                binding.terminal   = terminal;  // the first: with the pointer gone, no later event finds the binding
                binding.connection = nullptr;
                connection->release();
            }
            return;
        }
        // Not bound: the application owns it from `Accepted` on, and `accept` takes it from the pending ones.
        const auto pending = std::ranges::find(pending_accepts_, connection);
        const bool owned   = pending != pending_accepts_.end() || events.accepted();
        if (!owned) {
            return;
        }
        if (terminal) {
            if (pending != pending_accepts_.end()) {
                pending_accepts_.erase(pending);
            }
            connection->release();
            return;
        }
        if (pending == pending_accepts_.end()) {
            pending_accepts_.push_back(connection);
        }
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
            {"accept",     &Runner::call_accept    },
            {"connect",    &Runner::call_connect   },
            {"write",      &Runner::call_write     },
            {"send",       &Runner::call_write     },
            {"read",       &Runner::call_read      },
            {"recv",       &Runner::call_read      },
            {"close",      &Runner::call_close     },
            {"shutdown",   &Runner::call_shutdown  },
        });
        for (const auto& [name, bound] : calls) {
            if (name == call.name) {
                return std::invoke(bound, this, event, call);
            }
        }
        return fail(event, std::format("unbound call {}", call.name));
    }

    Runner::Outcome Runner::call_socket(const Event& event, const CallLine& call) {
        return open_descriptor(event, call, call.result, Binding{});
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

    Runner::Outcome Runner::call_accept(const Event& event, const CallLine& call) {
        return wait(event, call, &Runner::try_accept);
    }

    Runner::Outcome Runner::call_connect(const Event& event, const CallLine& call) {
        auto found = descriptor_of(event, call, Binding::Kind::Socket);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        const bool in_progress = call.result == -1 && call.error == "EINPROGRESS";
        if (call.result != 0 && !in_progress) {
            return fail(event, "connect: unsupported result");
        }
        const HarnessConfig& config = harness_->config();
        const auto connected =
            harness_->tcp().connect(tcp::Endpoint{.address = config.peer_address, .port = config.peer_port}, now_);
        if (!connected) {
            return fail(event, std::format("connect: {}", name_of(connected.error())));
        }
        Binding& binding   = **found;
        binding.kind       = Binding::Kind::Connection;
        binding.connection = *connected;
        return {};
    }

    Runner::Outcome Runner::call_write(const Event& event, const CallLine& call) {
        auto found = live_connection(event, call);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        auto count = count_of(event, call);
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (auto flags = expect_no_flags(event, call); !flags) {
            return flags;
        }
        Harness::Connection& connection = *(*found)->connection;
        std::size_t written             = 0;
        while (written < *count) {
            const auto span = connection.prepare(*count - written);
            if (!span) {
                break;  // nothing writable: the peer's window is full, or no packet is free
            }
            std::ranges::copy(frames::pattern(span->size(), static_cast<std::uint8_t>(written)), span->begin());
            if (!connection.commit(span->size())) {
                break;
            }
            written += span->size();
        }
        if (std::cmp_not_equal(written, call.result)) {
            return fail(event,
                        std::format("{}: accepted {} of {}, expected {}", call.name, written, *count, call.result));
        }
        return {};
    }

    Runner::Outcome Runner::call_read(const Event& event, const CallLine& call) {
        return wait(event, call, &Runner::try_read);
    }

    Runner::Outcome Runner::call_close(const Event& event, const CallLine& call) {
        auto found = descriptor(event, call);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        if (auto zero = expect_zero(event, call); !zero) {
            return zero;
        }
        const std::int64_t number = call.arguments.front().integer;
        Binding& binding          = **found;
        if (binding.kind == Binding::Kind::Closed) {
            return fail(event, std::format("close: descriptor {} is already closed", number));
        }
        if (binding.kind == Binding::Kind::Connection && binding.connection != nullptr) {
            binding.connection->close();
            binding.kind = Binding::Kind::Closed;  // the slot stays until its terminal event releases it
            return {};
        }
        if (binding.kind == Binding::Kind::Listener) {
            harness_->tcp().unlisten(binding.port);
        }
        descriptors_.erase(number);
        return {};
    }

    Runner::Outcome Runner::call_shutdown(const Event& event, const CallLine& call) {
        auto found = live_connection(event, call);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        const std::string_view how = call.arguments.size() > 1 ? call.arguments[1].text : std::string_view{};
        if (how != "SHUT_WR") {
            return fail(event, std::format("unbound call shutdown({})", how));
        }
        if (auto zero = expect_zero(event, call); !zero) {
            return zero;
        }
        (*found)->connection->close();  // a FIN after what was committed; the connection stays readable
        return {};
    }

    Runner::Outcome Runner::wait(const Event& event, const CallLine& call, const Try attempt) {
        const Window window = window_of(event);
        Attempt satisfied   = std::invoke(attempt, this, event, call);
        while (satisfied && !*satisfied && window.low != window.high && now_ < window.high) {
            if (auto stepped = step(event); !stepped) {
                return stepped;
            }
            satisfied = std::invoke(attempt, this, event, call);
        }
        if (!satisfied) {
            return std::unexpected{std::move(satisfied.error())};
        }
        if (!*satisfied) {
            return fail(event, std::format("{}: not satisfied: {}", call.name, found_at(call)));
        }
        return {};
    }

    Runner::Attempt Runner::try_accept(const Event& event, const CallLine& call) {
        if (auto found = descriptor_of(event, call, Binding::Kind::Listener); !found) {
            return std::unexpected{std::move(found.error())};
        }
        if (pending_accepts_.empty()) {
            return false;
        }
        Binding accepted{};
        accepted.kind        = Binding::Kind::Connection;
        accepted.connection  = pending_accepts_.front();
        accepted.established = true;
        if (auto opened = open_descriptor(event, call, call.result, accepted); !opened) {
            return std::unexpected{std::move(opened.error())};
        }
        pending_accepts_.pop_front();
        return true;
    }

    Runner::Attempt Runner::try_read(const Event& event, const CallLine& call) {
        // Data can be asked only of a live connection; an errno word may name the event that released it.
        auto found =
            call.result < 0 ? descriptor_of(event, call, Binding::Kind::Connection) : live_connection(event, call);
        if (!found) {
            return std::unexpected{std::move(found.error())};
        }
        auto count = count_of(event, call);
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (auto flags = expect_no_flags(event, call); !flags) {
            return std::unexpected{std::move(flags.error())};
        }
        const Binding& binding = **found;
        if (call.result < 0) {
            return errno_holds(event, call, binding);
        }
        Harness::Connection& connection = *binding.connection;
        if (call.result == 0) {
            return connection.peer_closed() && connection.unread().empty();
        }
        const std::size_t available = std::min(connection.unread().size(), *count);
        if (available == 0) {
            return false;
        }
        if (std::cmp_not_equal(available, call.result)) {
            return fail(event, std::format("{}: {} unread, expected {}", call.name, available, call.result));
        }
        connection.consume(available);
        return true;
    }

    Runner::Attempt Runner::errno_holds(const Event& event, const CallLine& call, const Binding& binding) {
        if (call.result != -1) {
            return fail(event, std::format("{}: unsupported result {}", call.name, call.result));
        }
        const std::string_view word = call.error;
        if (word == "EAGAIN") {
            const Harness::Connection* connection = binding.connection;
            return connection != nullptr && connection->unread().empty() && !connection->peer_closed();
        }
        if (word == "ETIMEDOUT") {
            return binding.terminal == stream::Event::TimedOut;
        }
        if (word == "ECONNRESET") {
            return binding.terminal == stream::Event::Reset && binding.established;
        }
        if (word == "ECONNREFUSED") {
            return binding.terminal == stream::Event::Reset && !binding.established;
        }
        if (word == "EHOSTUNREACH") {
            return binding.terminal == stream::Event::Unresolved;
        }
        return fail(event, std::format("unbound errno {}", word));
    }

    std::string Runner::found_at(const CallLine& call) const {
        const auto found = descriptors_.find(call.arguments.front().integer);
        if (found == descriptors_.end()) {
            return "the descriptor is not open";
        }
        const Binding& binding = found->second;
        if (binding.kind == Binding::Kind::Listener) {
            return "no accepted connection is pending";
        }
        if (binding.connection == nullptr) {
            return std::format("the connection is {}{}",
                               name_of(binding.terminal.value_or(stream::Event::Closed)),
                               binding.established ? "" : ", never established");
        }
        const Harness::Connection& connection = *binding.connection;
        return std::format("{} unread, the peer {}, state {}",
                           connection.unread().size(),
                           connection.peer_closed() ? "closed" : "open",
                           name_of(connection.state()));
    }

    Runner::Outcome Runner::open_descriptor(const Event& event,
                                            const CallLine& call,
                                            const std::int64_t number,
                                            const Binding binding) {
        if (number < 0) {
            return fail(event, std::format("{}: result {} is not a descriptor", call.name, number));
        }
        if (!descriptors_.try_emplace(number, binding).second) {
            return fail(event, std::format("descriptor {} is in use", number));
        }
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

    std::expected<Runner::Binding*, Failure> Runner::live_connection(const Event& event, const CallLine& call) {
        auto found = descriptor_of(event, call, Binding::Kind::Connection);
        if (found && (*found)->connection == nullptr) {
            const stream::Event terminal = (*found)->terminal.value_or(stream::Event::Closed);
            return fail(event, std::format("{}: the connection is {}", call.name, name_of(terminal)));
        }
        return found;
    }

    std::expected<std::size_t, Failure> Runner::count_of(const Event& event, const CallLine& call) {
        constexpr std::size_t position = 2;  // write(n, ..., count)
        if (call.arguments.size() <= position || call.arguments[position].kind != Argument::Kind::Integer ||
            call.arguments[position].integer < 0) {
            return fail(event, std::format("{}: the count is not a number", call.name));
        }
        return static_cast<std::size_t>(call.arguments[position].integer);
    }

    Runner::Outcome Runner::expect_no_flags(const Event& event, const CallLine& call) {
        constexpr std::size_t position = 3;  // send(n, ..., count, flags)
        if (call.arguments.size() > position &&
            (call.arguments[position].kind != Argument::Kind::Integer || call.arguments[position].integer != 0)) {
            return fail(event, std::format("{}: flags other than 0 are not supported", call.name));
        }
        return {};
    }

    std::string_view Runner::kind_name(const Binding::Kind kind) noexcept {
        switch (kind) {
            case Binding::Kind::Socket:
                return "socket";
            case Binding::Kind::Listener:
                return "listener";
            case Binding::Kind::Connection:
                return "connection";
            case Binding::Kind::Closed:
                return "closed connection";
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
