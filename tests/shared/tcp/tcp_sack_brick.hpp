#pragma once

#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

namespace aloe::testing {

    /// The sum of every drop counter.
    [[nodiscard]] inline std::uint64_t drops(const tcp::TcpCounters& counters) {
        return counters.dropped_bad_header + counters.dropped_bad_checksum + counters.dropped_no_connection +
               counters.dropped_closed + counters.dropped_unexpected + counters.dropped_duplicate +
               counters.dropped_out_of_order + counters.dropped_out_of_window + counters.dropped_no_slot +
               counters.dropped_no_node + counters.dropped_table_full + counters.dropped_paws +
               counters.dropped_reset_challenged;
    }

    /// The brick under the recording policy: an unbounded window, so no rate reduction holds a resend back, and the
    /// hooks it heard, so a test sees which hook the stack called, when, and with what.
    class TcpRecordedFixture : public ::testing::TestWithParam<fabric::EmulatedOffloads>,
                               protected detail::FabricPair,
                               protected TcpHarness<fabric::Port, RecordingCongestionControl> {
    protected:
        TcpRecordedFixture()
            : FabricPair{GetParam()},
              TcpHarness{fabric, harness, port} {
        }
    };

    /**
     * @brief What the brick tests of the scoreboard, RACK and the tail probe share, over any fixture: a peer that
     * offers timestamps and SACK-permitted, so a segment carries 1448 bytes; a passive open with a round trip of 10 ms,
     * which gives an srtt of 10 ms, an RTO of 210 ms and RACK a minimum round trip of 10 ms; sends of 100 bytes
     * collected as `sent`; and `at(n)`, the stack's sequence `n` past the first of them.
     */
    template <typename Fixture>
    class SackBrick : public Fixture {
    protected:
        using Connection = Fixture::Connection;
        using Block      = wire::TcpSackBlock;

        SackBrick() {
            this->peer_.set_timestamps(100);
            this->peer_.set_sack_permitted(true);
        }

        /// `count` sends of 100 bytes, collected into `into`; false, with a failure added, when they did not all leave.
        [[nodiscard]] bool send(Connection& c, const int count, std::vector<frames::ParsedFrame>& into) {
            for (int sent_count = 0; sent_count < count; ++sent_count) {
                if (c.send(frames::pattern(100)) != 100U) {
                    ADD_FAILURE() << "send " << sent_count << " was not accepted whole";
                    return false;
                }
            }
            into = this->collect();
            if (into.size() != static_cast<std::size_t>(count)) {
                ADD_FAILURE() << "expected " << count << " segments, got " << into.size();
                return false;
            }
            return true;
        }

        /// A passive open with a round trip of 10 ms, then five sends at 10 ms into `sent`; null on failure.
        [[nodiscard]] Connection* open_and_send() {
            Connection* c = this->open_passive(this->peer_, std::chrono::milliseconds{10});
            return c != nullptr && send(*c, 5, sent) ? c : nullptr;
        }

        [[nodiscard]] wire::TcpSequence at(const std::uint32_t offset) const {
            return sent[0].tcp->sequence + offset;
        }

        [[nodiscard]] Block range(const std::uint32_t first, const std::uint32_t end) const {
            return Block{.first = at(first), .end = at(end)};
        }

        /// A pure ACK of `ack` with no SACK option.
        [[nodiscard]] std::vector<std::byte> ack_of(const wire::TcpSequence ack) {
            return this->peer_.segment({wire::TcpFlag::Ack}, {}, this->peer_.snd_nxt(), ack);
        }

        [[nodiscard]] const tcp::TcpCounters& counters() const {
            return this->tcp_->counters();
        }

        std::vector<frames::ParsedFrame> sent;
    };

}  // namespace aloe::testing
