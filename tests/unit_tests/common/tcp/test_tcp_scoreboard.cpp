#include <algorithm>
#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>
#include <tcp_sack_brick.hpp>

// The scoreboard: the two marks on the retransmission queue's entries, reported received and lost, their totals, what
// is in flight, the delivered count and the duplicate report's belief, and where the resend a lost mark owes stands;
// then the brick reading every acceptable ACK's SACK blocks into them.
namespace {

    using aloe::wire::TcpSequence;
    using Block  = aloe::wire::TcpSackBlock;
    using Blocks = std::vector<Block>;
    using Pool   = aloe::tcp::detail::SentSegmentPool<aloe::fabric::Packet>;
    using Queue  = aloe::tcp::detail::RetransmissionQueue<aloe::fabric::Packet>;
    using Entry  = Queue::Entry;
    using Report = Queue::DuplicateReport;
    using aloe::tcp::detail::no_node;

    [[nodiscard]] Block B(const std::uint32_t first, const std::uint32_t end) {
        return Block{.first = TcpSequence{first}, .end = TcpSequence{end}};
    }

    /// The queue over a pool of 16 nodes; most tests push five entries of 100 from 1000.
    class TcpScoreboard : public ::testing::Test {
    protected:
        /// An entry of `length` payload bytes at `first` on `into`, with an empty packet, linked at the tail and
        /// stamped once. Throws when no node is free, which fails the test.
        std::uint32_t push_to(Queue& into, const std::uint32_t first, const std::uint32_t length) {
            const std::uint32_t index = into.push(pool,
                                                  Entry{.first   = TcpSequence{first},
                                                        .length  = length,
                                                        .payload = static_cast<std::uint16_t>(length)})
                                            .value();
            into.sent(pool.node(index), now);
            return index;
        }

        std::uint32_t push(const std::uint32_t first, const std::uint32_t length) {
            return push_to(queue, first, length);
        }

        /// Five entries of 100 from 1000 on `into`; their nodes in `nodes`, oldest first.
        void push_five(Queue& into) {
            for (std::uint32_t position = 0; position < 5; ++position) {
                nodes[position] = push_to(into, 1000 + (position * 100), 100);
            }
        }

        void push_five() {
            push_five(queue);
        }

        /// One more transmission of the entry on `node`: a resend.
        void resend(const std::uint32_t node) {
            queue.sent(pool.node(node), now);
        }

        [[nodiscard]] const Entry& entry(const std::uint32_t node) const {
            return pool.node(node);
        }

        /// A report on `into` whose callback records the first sequence of each entry it runs for, and whether that
        /// entry was already marked when it ran.
        Queue::Reported report(Queue& into, const Block& block) {
            return into.report(pool, block, [this](const Entry& reported) {
                called.push_back(reported.first.value);
                called_after_mark = called_after_mark || reported.reported;
            });
        }

        Queue::Reported report(const Block& block) {
            return report(queue, block);
        }

        /// A cumulative ACK of `ack` with SND.UNA at `una`; the callback records as `report`'s does.
        Queue::Freed acknowledge(const std::uint32_t ack, const std::uint32_t una) {
            return queue.acknowledge(pool, TcpSequence{ack}, TcpSequence{una}, [this](const Entry& freed) {
                called.push_back(freed.first.value);
            });
        }

        /// A packet as a resend leaves it: `payload` bytes behind the frame's headers.
        [[nodiscard]] aloe::fabric::Packet framed(const std::uint16_t payload) {
            aloe::fabric::Packet packet = buffers.allocate().value();
            std::ignore                 = packet.append(frame_headers + payload).value();
            return packet;
        }

        /// The head the shrink tests start from, 100 bytes at 1000 in a framed packet, then four entries of 100.
        void push_framed_head() {
            nodes[0] = queue
                           .push(pool,
                                 Entry{.packet  = framed(100),
                                       .first   = TcpSequence{1000},
                                       .length  = 100,
                                       .payload = 100,
                                       .framed  = true})
                           .value();
            queue.sent(pool.node(nodes[0]), now);
            for (std::uint32_t position = 1; position < 5; ++position) {
                nodes[position] = push(1000 + (position * 100), 100);
            }
        }

        /// The head's packet stays lent, the returned loan, while its copy of `[first, 1100)` came back: as the copy
        /// fallback leaves an entry whose original the device still holds.
        [[nodiscard]] aloe::fabric::Packet copy_came_back(const std::uint32_t first) {
            Entry& head               = pool.node(nodes[0]);
            aloe::fabric::Packet loan = head.packet.lend();
            const auto payload        = static_cast<std::uint16_t>(1100 - first);
            head.copy                 = framed(payload);
            head.copy_first           = TcpSequence{first};
            head.copy_payload         = payload;
            return loan;
        }

        static constexpr std::size_t frame_headers = 54;  ///< Ethernet, IPv4 and TCP without options.

        aloe::fabric::Pool buffers{8, 2048};  ///< Before `pool`, whose entries give their packets back to it.
        Pool pool{16};
        Queue queue{};
        aloe::core::TimePoint now{};
        std::array<std::uint32_t, 5> nodes{};
        std::vector<std::uint32_t> called;
        bool called_after_mark = false;
    };

    using aloe::testing::drops;

    /// The brick with the shared helpers (`aloe::testing::SackBrick`): a peer that offers timestamps and SACK-permitted
    /// unless the test says not, five sends of 100 bytes collected as `sent`, and `at(n)`, the stack's sequence `n`
    /// past the first of them. The passive open takes a round trip of 10 ms, so RACK's reordering window is 2.5 ms and
    /// a block at the instant of the sends marks nothing lost: RACK's round trip of zero for the entry it reports
    /// leaves the entries before it 2.5 ms to arrive. At a round trip of zero the window would be zero too, and RACK
    /// would mark them at once.
    using TcpScoreboardBrick = aloe::testing::SackBrick<aloe::testing::TcpFixture>;

    /// The randomized test's stream: the byte at each offset from its first, with no period a misplaced segment could
    /// match.
    [[nodiscard]] std::byte stream_byte(const std::uint32_t offset) noexcept {
        return std::byte{static_cast<std::uint8_t>(offset ^ (offset >> 8U) ^ (offset >> 16U))};
    }

    /// A part of the stream, as offsets from its first byte.
    struct Range {
        std::uint32_t first = 0;
        std::uint32_t end   = 0;
    };

    /**
     * @brief The randomized test's receiver: what it holds of the stream, as ranges of offsets. It answers each segment
     * with a cumulative ACK and up to four blocks, as many as the option space leaves, the newest first; a segment it
     * already had with a duplicate report in front of them (RFC 2883), then, when that segment lies above the ACK, the
     * block that holds it. It may forget its highest range, as a receiver that releases what it held and had reported.
     */
    class ModelReceiver {
    public:
        struct Answer {
            std::uint32_t ack = 0;
            std::vector<Range> blocks;
        };

        /// `blocks`, the most an answer carries: four, or three beside a timestamp option.
        explicit ModelReceiver(const std::size_t blocks) noexcept
            : blocks_{blocks} {
        }

        /// When `forget`, the highest range held above the ACK is forgotten first; then the segment of `payload` at
        /// `first` is taken in and answered.
        [[nodiscard]] Answer
        receive(const std::uint32_t first, const std::span<const std::byte> payload, const bool forget) {
            if (forget && !held_.empty()) {
                held_.erase(std::ranges::max_element(held_, {}, &Range::first));
                ++forgotten_;
            }
            const auto end = static_cast<std::uint32_t>(first + payload.size());
            for (std::uint32_t at = 0; at < payload.size(); ++at) {
                if (payload[at] != stream_byte(first + at)) {
                    ++corrupt_;
                    break;
                }
            }
            Answer answer{};
            std::optional<Range> holder{};
            if (end <= next_) {
                answer.blocks.push_back(Range{.first = first, .end = end});
            } else if (const auto found = std::ranges::find_if(
                           held_,
                           [first, end](const Range& range) { return range.first <= first && end <= range.end; });
                       found != held_.end()) {
                holder = *found;
                answer.blocks.push_back(Range{.first = first, .end = end});
                answer.blocks.push_back(*holder);
            } else {
                take(Range{.first = std::max(first, next_), .end = end});
            }
            answer.ack = next_;
            for (const Range& range : held_) {
                if (answer.blocks.size() == blocks_) {
                    break;
                }
                if (!holder || range.first != holder->first) {
                    answer.blocks.push_back(range);
                }
            }
            return answer;
        }

        /// Everything before it was received in order.
        [[nodiscard]] std::uint32_t next() const noexcept {
            return next_;
        }

        /// Segments with a byte that is not the stream's at its offset.
        [[nodiscard]] std::uint64_t corrupt() const noexcept {
            return corrupt_;
        }

        [[nodiscard]] std::uint64_t forgotten() const noexcept {
            return forgotten_;
        }

    private:
        /// `range` merged with every range it touches, the newest; RCV.NXT moves past it when it reaches it.
        void take(Range range) {
            for (auto at = held_.begin(); at != held_.end();) {
                if (at->end < range.first || range.end < at->first) {
                    ++at;
                    continue;
                }
                range.first = std::min(range.first, at->first);
                range.end   = std::max(range.end, at->end);
                at          = held_.erase(at);
            }
            if (range.first <= next_) {
                next_ = std::max(next_, range.end);
                return;
            }
            held_.insert(held_.begin(), range);
        }

        std::size_t blocks_ = 0;
        std::uint32_t next_ = 0;
        std::vector<Range> held_;  ///< Above `next_`, disjoint, the newest first.
        std::uint64_t corrupt_   = 0;
        std::uint64_t forgotten_ = 0;
    };

    /**
     * @brief The randomized test's path from the sender to the receiver: every segment arrives `delay` steps after it
     * left, in order, but 5 in 100 are dropped, 2 in 100 arrive twice and 10 in 100 are held back behind up to four
     * later segments, or, when fewer follow, `longest_hold` steps at most. The ACKs come back at once, so a round trip
     * is `delay` steps.
     */
    class ModelChannel {
    public:
        static constexpr int delay        = 5;
        static constexpr int longest_hold = 4;

        void send(aloe::frames::ParsedFrame frame, const int step, std::mt19937& engine) {
            std::uniform_int_distribution<int> percent{0, 99};
            if (percent(engine) < 5) {
                ++dropped_;
                return;
            }
            const bool twice  = percent(engine) < 2;
            const int behind  = percent(engine) < 10 ? std::uniform_int_distribution<int>{1, 4}(engine) : 0;
            duplicated_      += twice ? 1U : 0U;
            held_            += behind > 0 ? 1U : 0U;
            if (twice) {
                wire_.push_back({.frame = frame, .due = step + delay, .behind = behind});
            }
            wire_.push_back({.frame = std::move(frame), .due = step + delay, .behind = behind});
        }

        /// What arrives at `step`, in the order it arrives: each segment due, in the order they left, but one held
        /// back, which follows the last of the segments it waited for, or arrives `longest_hold` steps late.
        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> deliver(const int step) {
            std::vector<aloe::frames::ParsedFrame> arrived;
            for (bool moved = true; moved;) {
                moved = false;
                for (auto at = wire_.begin(); at != wire_.end() && at->due <= step; ++at) {
                    if (at->behind > 0 && step - at->due < longest_hold) {
                        continue;
                    }
                    for (auto waiting = wire_.begin(); waiting != at; ++waiting) {
                        waiting->behind -= waiting->behind > 0 ? 1 : 0;
                    }
                    arrived.push_back(std::move(at->frame));
                    wire_.erase(at);
                    moved = true;
                    break;
                }
            }
            return arrived;
        }

        [[nodiscard]] std::uint64_t dropped() const noexcept {
            return dropped_;
        }

        [[nodiscard]] std::uint64_t duplicated() const noexcept {
            return duplicated_;
        }

        [[nodiscard]] std::uint64_t held() const noexcept {
            return held_;
        }

    private:
        struct InTransit {
            aloe::frames::ParsedFrame frame{};
            int due    = 0;
            int behind = 0;  ///< Later segments still to arrive before it.
        };

        std::deque<InTransit> wire_;
        std::uint64_t dropped_    = 0;
        std::uint64_t duplicated_ = 0;
        std::uint64_t held_       = 0;
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpScoreboardBrick,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_F(TcpScoreboard, ABlockMarksTheEntriesItCoversWhole) {
    push_five();
    const Queue::Reported reported = report(B(1100, 1300));
    EXPECT_FALSE(entry(nodes[0]).reported);
    EXPECT_TRUE(entry(nodes[1]).reported);
    EXPECT_TRUE(entry(nodes[2]).reported);
    EXPECT_FALSE(entry(nodes[3]).reported);
    EXPECT_FALSE(entry(nodes[4]).reported);
    EXPECT_EQ(reported.bytes, 200U);
    EXPECT_FALSE(reported.reordering);
    EXPECT_EQ(queue.reported_bytes(), 200U);
    EXPECT_EQ(queue.reported_segments(), 2U);
    EXPECT_EQ(queue.in_flight_bytes(500), 300U);
    EXPECT_EQ(called, (std::vector<std::uint32_t>{1100, 1200}));
    EXPECT_FALSE(called_after_mark) << "the callback runs before the mark";
    EXPECT_EQ(queue.size(), 5U) << "a reported entry stays queued";
}

TEST_F(TcpScoreboard, ABlockThatCoversPartOfAnEntryMarksNothing) {
    push_five();
    EXPECT_EQ(report(B(1150, 1250)).bytes, 0U);
    EXPECT_EQ(queue.reported_segments(), 0U);
    EXPECT_TRUE(called.empty());
    EXPECT_EQ(report(B(1050, 1300)).bytes, 200U) << "the entries it covers whole, and not the one it starts inside";
    EXPECT_FALSE(entry(nodes[0]).reported);
    EXPECT_TRUE(entry(nodes[1]).reported);
    EXPECT_TRUE(entry(nodes[2]).reported);
    EXPECT_FALSE(entry(nodes[3]).reported);
    EXPECT_EQ(queue.reported_bytes(), 200U);
}

TEST_F(TcpScoreboard, ABlockReportedTwiceCountsOnce) {
    push_five();
    EXPECT_EQ(report(B(1100, 1300)).bytes, 200U);
    called.clear();
    const Queue::Reported again = report(B(1100, 1300));
    EXPECT_EQ(again.bytes, 0U);
    EXPECT_TRUE(called.empty());
    EXPECT_EQ(queue.reported_bytes(), 200U);
    EXPECT_EQ(queue.reported_segments(), 2U);
    EXPECT_EQ(queue.delivered(), 200U);
}

TEST_F(TcpScoreboard, ANeverResentEntryBelowTheHighestIsReordering) {
    push_five();
    EXPECT_FALSE(report(B(1300, 1400)).reordering) << "nothing reported below it";
    EXPECT_TRUE(report(B(1100, 1200)).reordering) << "sent once, and reported after a later entry";

    Queue resent{};
    push_five(resent);
    resent.sent(pool.node(nodes[1]), now);  // the second entry left twice
    EXPECT_FALSE(report(resent, B(1300, 1400)).reordering);
    EXPECT_FALSE(report(resent, B(1100, 1200)).reordering) << "a resend's report may answer the resend";
    resent.drop(pool);
}

TEST_F(TcpScoreboard, ALostEntryStopsBeingLostWhenReported) {
    push_five();
    queue.mark_lost(pool, nodes[1]);
    EXPECT_EQ(queue.lost_bytes(), 100U);
    std::ignore = report(B(1100, 1200));
    EXPECT_EQ(queue.lost_bytes(), 0U);
    EXPECT_FALSE(entry(nodes[1]).lost);
    EXPECT_TRUE(entry(nodes[1]).reported);
    EXPECT_EQ(queue.first_lost(pool), no_node);
}

TEST_F(TcpScoreboard, MarkLostSkipsWhatIsReportedOrAlreadyLost) {
    push_five();
    EXPECT_EQ(queue.first_lost(pool), no_node);
    std::ignore = report(B(1100, 1200));
    queue.mark_lost(pool, nodes[1]);
    queue.mark_lost(pool, nodes[0]);
    queue.mark_lost(pool, nodes[1]);
    queue.mark_lost(pool, nodes[0]);
    EXPECT_EQ(queue.lost_bytes(), 100U);
    EXPECT_FALSE(entry(nodes[1]).lost) << "a reported entry is not lost";
    EXPECT_TRUE(entry(nodes[0]).lost);
    EXPECT_EQ(queue.first_lost(pool), nodes[0]);
}

TEST_F(TcpScoreboard, InFlightIsTheFlightLessReportedLessLost) {
    push_five();
    std::ignore = report(B(1100, 1200));
    queue.mark_lost(pool, nodes[2]);
    EXPECT_EQ(queue.in_flight_bytes(500), 300U);
    queue.clear_lost(pool, nodes[2]);  // its resend left
    EXPECT_EQ(queue.in_flight_bytes(500), 400U);
    EXPECT_EQ(queue.lost_bytes(), 0U);
    EXPECT_FALSE(entry(nodes[2]).lost);
}

TEST_F(TcpScoreboard, ACumulativeAckKeepsTheTotalsRight) {
    push_five();
    std::ignore = report(B(1100, 1300));
    queue.mark_lost(pool, nodes[0]);
    called.clear();
    const Queue::Freed freed = acknowledge(1300, 1000);
    EXPECT_EQ(freed.length, 300U) << "three entries freed";
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.reported_bytes(), 0U);
    EXPECT_EQ(queue.reported_segments(), 0U);
    EXPECT_EQ(queue.lost_bytes(), 0U);
    EXPECT_EQ(called, (std::vector<std::uint32_t>{1000})) << "only the entry no block reported";
    EXPECT_EQ(queue.in_flight_bytes(200), 200U);
}

TEST_F(TcpScoreboard, DeliveredCountsEachByteOnce) {
    push_five();
    std::ignore = report(B(1100, 1200));
    EXPECT_EQ(queue.delivered(), 100U) << "the block reported it first";
    std::ignore = acknowledge(1200, 1000);
    EXPECT_EQ(queue.delivered(), 200U) << "the ACK adds what no block reported";
}

TEST_F(TcpScoreboard, ADuplicateReportBelowTheAckIsRecognised) {
    push_five();
    resend(nodes[0]);
    resend(nodes[1]);
    EXPECT_EQ(queue.duplicate_report(B(900, 1000), std::nullopt, TcpSequence{1000}, 65535, 1460), Report::Believed);
    std::ignore = acknowledge(1500, 1000);
    ASSERT_TRUE(queue.empty());
    EXPECT_EQ(queue.duplicate_report(B(1400, 1500), std::nullopt, TcpSequence{1500}, 65535, 1460), Report::Believed)
        << "from the option alone: with nothing outstanding too";
}

TEST_F(TcpScoreboard, ADuplicateReportInsideTheSecondBlockIsRecognised) {
    push_five();
    resend(nodes[0]);
    EXPECT_EQ(queue.duplicate_report(B(1200, 1300), B(1100, 1400), TcpSequence{1000}, 65535, 1460), Report::Believed);
    EXPECT_EQ(queue.duplicate_report(B(1200, 1300), B(1300, 1400), TcpSequence{1000}, 65535, 1460), Report::None)
        << "a second block that does not contain the first: an ordinary block";
}

TEST_F(TcpScoreboard, ADuplicateReportThatCannotBeTrueIsNotBelieved) {
    push_five();
    EXPECT_EQ(queue.duplicate_report(B(900, 1000), std::nullopt, TcpSequence{1000}, 65535, 1460), Report::Disbelieved)
        << "nothing was ever resent";
    for (int count = 0; count < 99; ++count) {
        resend(nodes[0]);  // so many resends that the count no longer refuses what follows
    }
    EXPECT_EQ(queue.duplicate_report(B(1000U - 70000U, 1000), std::nullopt, TcpSequence{1000}, 65535, 1460),
              Report::None)
        << "longer than any window the peer offered: no report at all";
    EXPECT_EQ(queue.duplicate_report(B(1000U - 65535U, 1000), std::nullopt, TcpSequence{1000}, 65535, 1460),
              Report::Believed)
        << "as long as the largest window: believed";
}

TEST_F(TcpScoreboard, ADuplicateReportNamesAReportedResend) {
    push_five();
    resend(nodes[1]);
    resend(nodes[3]);
    std::ignore = report(B(1100, 1300));  // the second entry, resent, and the third, sent once
    EXPECT_TRUE(queue.names_reported_resend(pool, B(1100, 1200)));
    EXPECT_FALSE(queue.names_reported_resend(pool, B(1200, 1300))) << "reported, but never resent";
    EXPECT_FALSE(queue.names_reported_resend(pool, B(1300, 1400))) << "resent, but not reported";
    EXPECT_FALSE(queue.names_reported_resend(pool, B(1150, 1200))) << "a part of it does not name it";
}

TEST_F(TcpScoreboard, ClearAllReportedIsReneging) {
    push_five();
    std::ignore = report(B(1000, 1200));
    ASSERT_TRUE(queue.head_reported(pool));
    queue.clear_all_reported(pool);
    for (const std::uint32_t node : nodes) {
        EXPECT_FALSE(entry(node).reported);
    }
    EXPECT_FALSE(queue.head_reported(pool));
    EXPECT_EQ(queue.reported_bytes(), 0U);
    EXPECT_EQ(queue.reported_segments(), 0U);
    EXPECT_EQ(queue.in_flight_bytes(500), 500U);
    EXPECT_FALSE(queue.highest_reported().has_value()) << "back to what the cumulative ACKs said: nothing yet";
}

TEST_F(TcpScoreboard, ClearAllLostIsAnUndo) {
    push_five();
    queue.mark_lost(pool, nodes[1]);
    queue.mark_lost(pool, nodes[3]);
    queue.mark_waiting(pool, nodes[3]);
    queue.clear_all_lost(pool);
    for (const std::uint32_t node : nodes) {
        EXPECT_FALSE(entry(node).lost);
        EXPECT_FALSE(entry(node).waiting);
    }
    EXPECT_EQ(queue.lost_bytes(), 0U);
    EXPECT_EQ(queue.first_lost(pool), no_node);
    EXPECT_FALSE(queue.waiting()) << "a resend nothing owes any more does not wait";
}

// What resends leave as the next episode begins: the resends out (Linux's `retrans_out`), and whether a resend of an
// episode is unacknowledged at all, out or marked lost again (`tcp_any_retrans_done`). A tail probe's second
// transmission, which owes no mark, is neither: it carries no undo stamp forward.
TEST_F(TcpScoreboard, AnEpisodesResendStaysUnacknowledgedUntilFreedAndAProbesIsNone) {
    push_five();
    resend(nodes[4]);  // the tail probed: sent twice, owing no mark
    EXPECT_EQ(queue.resends_out(pool), 0U);
    EXPECT_FALSE(queue.resent_unacknowledged(pool)) << "a probe's resend";
    queue.mark_lost(pool, nodes[1]);
    resend(nodes[1]);
    queue.clear_lost(pool, nodes[1]);  // the resend the mark owed left
    EXPECT_EQ(queue.resends_out(pool), 1U);
    EXPECT_TRUE(queue.resent_unacknowledged(pool));
    queue.mark_lost(pool, nodes[1]);  // found lost again: out no more, but unacknowledged still
    EXPECT_EQ(queue.resends_out(pool), 0U);
    EXPECT_TRUE(queue.resent_unacknowledged(pool));
    queue.mark_lost(pool, nodes[4]);        // the probed tail found lost: a first loss
    std::ignore = acknowledge(1200, 1000);  // frees the first two
    EXPECT_FALSE(queue.resent_unacknowledged(pool)) << "the probed tail, marked, was resent by no episode";
}

TEST_F(TcpScoreboard, AWaitEndsWhenTheResendLeavesABlockReportsOrAnAckFreesTheEntry) {
    push_five();
    EXPECT_FALSE(queue.waiting());
    for (std::uint32_t position = 0; position < 3; ++position) {
        queue.mark_lost(pool, nodes[position]);
        queue.mark_waiting(pool, nodes[position]);
    }
    queue.mark_waiting(pool, nodes[2]);  // again: the same wait
    EXPECT_TRUE(queue.waiting());
    queue.clear_lost(pool, nodes[0]);  // its resend left
    EXPECT_FALSE(entry(nodes[0]).waiting);
    EXPECT_TRUE(queue.waiting()) << "two others still wait";
    std::ignore = report(B(1100, 1200));
    EXPECT_FALSE(entry(nodes[1]).waiting) << "reported received: its resend is moot";
    EXPECT_TRUE(queue.waiting()) << "the third still waits";
    std::ignore = acknowledge(1300, 1000);  // frees the third with the two before it
    EXPECT_FALSE(queue.waiting());
    queue.mark_lost(pool, nodes[3]);
    queue.mark_waiting(pool, nodes[3]);
    queue.clear_waiting(pool, nodes[3]);  // the packet is back
    EXPECT_FALSE(queue.waiting());
    EXPECT_TRUE(entry(nodes[3]).lost) << "still owed";
}

TEST_F(TcpScoreboard, ANewLostMarkHasHadNoResendTried) {
    push_five();
    queue.mark_lost(pool, nodes[0]);
    EXPECT_FALSE(entry(nodes[0]).tried);
    queue.mark_tried(pool, nodes[0]);
    EXPECT_TRUE(entry(nodes[0]).tried);
    queue.mark_lost(pool, nodes[0]);  // already lost: the same mark
    EXPECT_TRUE(entry(nodes[0]).tried);
    queue.clear_lost(pool, nodes[0]);  // the resend left
    queue.mark_lost(pool, nodes[0]);   // and is lost in turn: a new mark
    EXPECT_FALSE(entry(nodes[0]).tried);
}

TEST_F(TcpScoreboard, AReportedHeadShrunkThenFreedLeavesTheTotalsAtZero) {
    push_framed_head();
    std::ignore = report(B(1000, 1100));
    std::ignore = acknowledge(1030, 1000);  // inside the reported head
    EXPECT_EQ(queue.reported_bytes(), 70U) << "what no cumulative ACK has covered";
    EXPECT_EQ(queue.in_flight_bytes(470), 400U) << "never more than the flight still counts of the head";
    const aloe::fabric::Packet loan = copy_came_back(1030);
    queue.reconcile(pool, nodes[0]);
    ASSERT_EQ(entry(nodes[0]).first, TcpSequence{1030}) << "the head shrank to its copy's range";
    EXPECT_EQ(queue.reported_bytes(), 70U);
    EXPECT_EQ(queue.in_flight_bytes(470), 400U);
    std::ignore = acknowledge(1060, 1030);
    queue.trim_to(pool, nodes[0], TcpSequence{1060});
    ASSERT_EQ(entry(nodes[0]).first, TcpSequence{1060}) << "trimmed to what the peer lacks";
    EXPECT_EQ(queue.reported_bytes(), 40U);
    EXPECT_EQ(queue.in_flight_bytes(440), 400U);
    std::ignore = acknowledge(1100, 1060);
    EXPECT_EQ(queue.reported_bytes(), 0U);
    EXPECT_EQ(queue.reported_segments(), 0U);
    EXPECT_EQ(queue.in_flight_bytes(400), 400U);
    EXPECT_EQ(queue.delivered(), 100U) << "the block delivered the head's bytes, once";
}

TEST_F(TcpScoreboard, ALostHeadShrunkThenFreedLeavesTheTotalsAtZero) {
    push_framed_head();
    queue.mark_lost(pool, nodes[0]);
    std::ignore = acknowledge(1030, 1000);  // inside the lost head
    EXPECT_EQ(queue.lost_bytes(), 70U) << "what no cumulative ACK has covered";
    EXPECT_EQ(queue.in_flight_bytes(470), 400U);
    const aloe::fabric::Packet loan = copy_came_back(1030);
    queue.reconcile(pool, nodes[0]);
    ASSERT_EQ(entry(nodes[0]).first, TcpSequence{1030});
    EXPECT_EQ(queue.lost_bytes(), 70U);
    EXPECT_EQ(queue.in_flight_bytes(470), 400U);
    std::ignore = acknowledge(1060, 1030);
    queue.trim_to(pool, nodes[0], TcpSequence{1060});
    ASSERT_EQ(entry(nodes[0]).first, TcpSequence{1060});
    EXPECT_EQ(queue.lost_bytes(), 40U);
    EXPECT_EQ(queue.in_flight_bytes(440), 400U);
    queue.clear_lost(pool, nodes[0]);  // its resend left
    EXPECT_EQ(queue.lost_bytes(), 0U);
    EXPECT_EQ(queue.in_flight_bytes(440), 440U);
    queue.mark_lost(pool, nodes[0]);  // and was lost again
    EXPECT_EQ(queue.lost_bytes(), 40U);
    std::ignore = acknowledge(1100, 1060);
    EXPECT_EQ(queue.lost_bytes(), 0U);
    EXPECT_EQ(queue.in_flight_bytes(400), 400U);
}

TEST_F(TcpScoreboard, LastSentSkipsAFinThatNeverLeft) {
    push_five();
    const std::uint32_t fin =
        queue
            .push(pool,
                  Entry{
                      .first  = TcpSequence{1500},
                      .length = 1,
                      .flags  = aloe::wire::TcpFlags{aloe::wire::TcpFlag::Fin, aloe::wire::TcpFlag::Ack}
    })
            .value();
    EXPECT_EQ(queue.last_sent(pool), nodes[4]) << "the FIN waits for the window";
    queue.sent(pool.node(fin), now);
    EXPECT_EQ(queue.last_sent(pool), fin);
}

TEST_F(TcpScoreboard, WrapsAcrossTheTopOfTheSequenceSpace) {
    for (std::uint32_t position = 0; position < 5; ++position) {
        nodes[position] = push(0xffffff80U + (position * 0x40U), 0x40);
    }
    EXPECT_EQ(report(B(0xffffffc0U, 0x00000040U)).bytes, 0x80U);
    EXPECT_FALSE(entry(nodes[0]).reported);
    EXPECT_TRUE(entry(nodes[1]).reported) << "0xffffffc0 to 0";
    EXPECT_TRUE(entry(nodes[2]).reported) << "0 to 0x40";
    EXPECT_FALSE(entry(nodes[3]).reported);
    EXPECT_EQ(queue.highest_reported(), TcpSequence{0x00000040U});
    EXPECT_TRUE(report(B(0xffffff80U, 0xffffffc0U)).reordering) << "below the highest, across the wrap";
    std::ignore = acknowledge(0x40U, 0xffffff80U);
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.reported_bytes(), 0U);
    EXPECT_EQ(queue.delivered(), 0xc0U) << "every byte once, across the wrap";
    EXPECT_EQ(queue.highest_reported(), TcpSequence{0x00000040U}) << "the cumulative ACK";
}

TEST_P(TcpScoreboardBrick, AnAckWithABlockMarksItsEntries) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    const std::size_t writable = c->writable();
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));
    EXPECT_FALSE(tcp_->sent_segment(*c, 1).reported);
    EXPECT_TRUE(tcp_->sent_segment(*c, 2).reported);
    EXPECT_FALSE(tcp_->sent_segment(*c, 3).reported);
    EXPECT_EQ(c->reported_bytes(), 100U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    EXPECT_EQ(c->in_flight(), 400U);
    EXPECT_EQ(tcp_->counters().sack_acks_received, 1U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_received, 0U);
    EXPECT_TRUE(collect().empty()) << "nothing is resent";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 0U);
    EXPECT_EQ(c->writable(), writable) << "the MSS before and after: the report only widened the room";
}

TEST_P(TcpScoreboardBrick, ABlockACorrectPeerNeverSendsMarksNothing) {
    peer_.set_timestamps(std::nullopt);  // four blocks take the option bytes a timestamp option would share
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    const std::uint64_t dropped = drops(tcp_->counters());
    receive(peer_.sack(at(0),
                       Blocks{
                           Block{.first = at(400),           .end = c->committed() + 100}, // past SND.NXT
                           range(300, 200), // reversed
                           Block{.first = c->acknowledged(), .end = at(100)             }, // starts at SND.UNA
                           range(100, 150), // half an entry
    }));
    for (std::size_t position = 0; position < 5; ++position) {
        EXPECT_FALSE(tcp_->sent_segment(*c, position).reported) << "entry " << position;
    }
    EXPECT_EQ(c->reported_bytes(), 0U);
    EXPECT_EQ(drops(tcp_->counters()), dropped) << "not an error";
    EXPECT_EQ(tcp_->counters().sack_acks_received, 1U) << "read, each block refused";
    EXPECT_EQ(tcp_->counters().duplicate_sacks_received, 0U);
    EXPECT_EQ(c->state(), aloe::tcp::State::Established);
}

TEST_P(TcpScoreboardBrick, BlocksOnAnAckBelowSndUnaAreRead) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    receive(ack_of(at(100)));
    ASSERT_EQ(c->acknowledged(), at(100));
    receive(peer_.sack(at(0), Blocks{range(300, 400)}));  // late: it acknowledges less than SND.UNA
    const auto& entry = tcp_->sent_segment(*c, 2);        // the first entry is freed
    EXPECT_EQ(entry.first, at(300));
    EXPECT_TRUE(entry.reported);
    EXPECT_EQ(c->reported_bytes(), 100U);
    EXPECT_EQ(c->acknowledged(), at(100)) << "the late ACK moves no SND.UNA";
    EXPECT_EQ(tcp_->counters().sack_acks_received, 1U);
}

TEST_P(TcpScoreboardBrick, BlocksAreIgnoredWithoutNegotiation) {
    peer_.set_sack_permitted(false);
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    ASSERT_FALSE(c->sack());
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));
    EXPECT_FALSE(tcp_->sent_segment(*c, 2).reported);
    EXPECT_EQ(c->reported_bytes(), 0U);
    EXPECT_EQ(c->in_flight(), 500U);
    EXPECT_EQ(tcp_->counters().sack_acks_received, 0U);
}

TEST_P(TcpScoreboardBrick, AReportedEntryKeepsItsPacketUntilTheCumulativeAck) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    const std::size_t nodes = tcp_->send_nodes_available();
    ASSERT_TRUE(send(*c, 5, sent));
    receive(peer_.sack(at(0), Blocks{range(200, 300)}));
    ASSERT_TRUE(tcp_->sent_segment(*c, 2).reported);
    EXPECT_FALSE(tcp_->sent_segment(*c, 2).packet.empty());
    EXPECT_EQ(c->queued_segments(), 5U);
    receive(ack_of(at(500)));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(tcp_->send_nodes_available(), nodes);
    EXPECT_EQ(c->reported_bytes(), 0U);
    EXPECT_EQ(c->in_flight(), 0U);
    EXPECT_EQ(c->delivered(), 500U) << "each byte once: 100 by the block, 400 by the ACK";
}

TEST_P(TcpScoreboardBrick, ADuplicateReportIsCountedOnceBelieved) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    receive(peer_.sack(at(100), Blocks{range(0, 100)}));  // the first segment, below the ACK: a duplicate report
    EXPECT_EQ(tcp_->counters().sack_acks_received, 1U);
    EXPECT_EQ(tcp_->counters().duplicate_sacks_received, 0U) << "nothing was resent: it cannot be true";
    EXPECT_EQ(c->acknowledged(), at(100));
    EXPECT_EQ(c->reported_bytes(), 0U);
}

TEST_P(TcpScoreboardBrick, AtTheRtoAReportedHeadMeansRenegingEveryMarkIsClearedAndTheHeadResent) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    receive(peer_.sack(at(0), Blocks{range(100, 200), range(300, 400)}));
    receive(ack_of(at(100)));  // the reported entry is the head now: the peer discarded it
    ASSERT_TRUE(tcp_->sent_segment(*c, 0).reported);
    ASSERT_EQ(c->reported_bytes(), 200U);
    advance(std::chrono::milliseconds{200});  // reneging armed the RTO short, at max(srtt / 2, 10 ms): 10 ms on
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the head, which the scoreboard no longer holds as received";
    EXPECT_EQ(resent[0].tcp->sequence, at(100));
    for (std::size_t position = 0; position < 4; ++position) {
        EXPECT_FALSE(tcp_->sent_segment(*c, position).reported) << "entry " << position;
    }
    EXPECT_EQ(c->reported_bytes(), 0U);
    EXPECT_EQ(c->lost_bytes(), 300U) << "everything is lost; the resend that left cleared the head's mark";
    EXPECT_EQ(c->in_flight(), 100U) << "the head alone";
    EXPECT_EQ(c->recovery_state(), aloe::tcp::RecoveryState::Loss);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().sack_reneging, 1U);
}

TEST_P(TcpScoreboardBrick, AReportedHeadAcknowledgedInPartAndResentKeepsTheTotalsRight) {
    auto* c = open_and_send();
    ASSERT_NE(c, nullptr);
    receive(peer_.sack(at(0), Blocks{range(100, 200)}));
    receive(ack_of(at(150)));  // inside the reported entry, which the ACK leaves at the head
    ASSERT_TRUE(tcp_->sent_segment(*c, 0).reported);
    EXPECT_EQ(c->reported_bytes(), 50U) << "what no cumulative ACK has covered";
    EXPECT_EQ(c->in_flight(), 300U) << "350 outstanding, 50 of them reported";
    advance(std::chrono::milliseconds{200});  // the short RTO of reneging resends the head, trimmed to what it lacks
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, at(150));
    EXPECT_EQ(tcp_->sent_segment(*c, 0).first, at(150));
    EXPECT_EQ(c->reported_bytes(), 0U) << "a reported head at the RTO is reneging: the received marks are cleared";
    EXPECT_EQ(c->lost_bytes(), 300U) << "and everything is lost: the three entries behind the head";
    EXPECT_EQ(c->in_flight(), 50U) << "what is left of the head, resent";
    // The peer got the resend: its ACK echoes the resend's timestamp, not the SYN-ACK's, so it answers no original.
    peer_.see(resent[0]);
    receive(ack_of(at(500)));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(c->reported_bytes(), 0U) << "nothing left in the totals once every entry is freed";
    EXPECT_EQ(c->in_flight(), 0U);
    EXPECT_EQ(tcp_->counters().undos_original_ack + tcp_->counters().undos_duplicate_sack, 0U);
}

// The sender's parts together under a randomized channel: 2000 steps of 1 ms over one connection whose peer is the
// model receiver, which answers every segment that reaches it; then again over a second connection, to a peer that
// offers timestamps, whose echoes undo what the first's reports alone cannot, and whose option leaves an answer room
// for three blocks where the first has four. The tail probe waits two round trips, its allowance at zero. In each of
// the first 1000 steps the application writes, one time in eight, a burst of up to eight segments of 1 to `mss()`
// bytes, as far as `writable()` allows: flights with tails, and gaps after them. The 1000 steps left cover a resend
// lost twice, two RTOs of 205 and 410 ms. After every step what is in flight is within what is unacknowledged, the
// totals agree with the marks of the entries, no segment that left starts an entry the scoreboard held as received,
// and every byte the receiver got is the stream's; at the end every byte written arrived in order, the queue is empty
// and the connection is open.
TEST_P(TcpScoreboardBrick, ARandomChannelNeverBreaksTheScoreboard) {
    constexpr int steps         = 2000;
    constexpr int writing_steps = 1000;
    configure_tcp({.tail_probe_ack_delay = aloe::core::Duration::zero()});
    std::mt19937 engine{20261121};  // NOLINT(cert-msc51-cpp): a fixed seed, so the run reproduces
    aloe::frames::TcpPeer stamped{aloe::testing::peer_spec(aloe::testing::tcp_peer_port + 1), TcpSequence{5000U}};
    stamped.set_timestamps(100);
    stamped.set_sack_permitted(true);
    peer_.set_timestamps(std::nullopt);  // four blocks take the option bytes a timestamp option would share

    struct Run {
        std::string_view name{};
        aloe::frames::TcpPeer* peer = nullptr;
        std::size_t blocks          = 0;
    };

    for (const Run& run : {
             Run{.name = "no timestamps, four blocks", .peer = &peer_,   .blocks = 4},
             Run{.name = "timestamps, three blocks",   .peer = &stamped, .blocks = 3}
    }) {
        SCOPED_TRACE(run.name);
        aloe::frames::TcpPeer& peer = *run.peer;
        auto* c                     = open_passive(peer, std::chrono::milliseconds{10});
        ASSERT_NE(c, nullptr);
        const TcpSequence base = c->committed();  // the stream's first byte
        const std::size_t mss  = c->mss();
        ModelReceiver receiver{run.blocks};
        ModelChannel channel;
        std::uint32_t offered         = 0;
        std::uint64_t resent_reported = 0;

        // A segment the stack sent: it must not start an entry marked received, which is read before anything else
        // moves the marks; then it takes the channel.
        const auto left = [&](aloe::frames::ParsedFrame frame, const int step) {
            if (!frame.tcp || aloe::frames::tcp_payload(frame).empty()) {
                return;
            }
            for (std::size_t position = 0; position < c->queued_segments(); ++position) {
                const auto& entry = tcp_->sent_segment(*c, position);
                if (!frame.tcp->sequence.before(entry.first) &&
                    frame.tcp->sequence.before(entry.first + entry.length)) {
                    resent_reported += entry.reported ? 1U : 0U;
                    break;
                }
            }
            channel.send(std::move(frame), step, engine);
        };

        for (int step = 0; step < steps && !HasFailure(); ++step) {
            SCOPED_TRACE(step);
            if (step < writing_steps && std::uniform_int_distribution<int>{0, 7}(engine) == 0) {
                const int writes = std::uniform_int_distribution<int>{1, 8}(engine);
                for (int write = 0; write < writes && c->writable() > 0; ++write) {
                    const std::size_t size =
                        std::min(std::uniform_int_distribution<std::size_t>{1, mss}(engine), c->writable());
                    std::vector<std::byte> bytes(size);
                    for (std::size_t at = 0; at < size; ++at) {
                        bytes[at] = stream_byte(offered + static_cast<std::uint32_t>(at));
                    }
                    offered += static_cast<std::uint32_t>(c->send(bytes));
                }
            }
            for (aloe::frames::ParsedFrame& frame : collect()) {
                left(std::move(frame), step);
            }
            for (const aloe::frames::ParsedFrame& frame : channel.deliver(step)) {
                const auto first = static_cast<std::uint32_t>(frame.tcp->sequence - base);
                // RFC 7323's TS.Recent: the value of a segment at or below RCV.NXT that is no older than the last.
                const std::optional<aloe::wire::TcpOptions> options =
                    aloe::wire::TcpOptions::parse(aloe::frames::tcp_options_of(frame));
                if (first <= receiver.next() && options && options->timestamp_value &&
                    (!peer.ts_seen() || static_cast<std::int32_t>(*options->timestamp_value - *peer.ts_seen()) >= 0)) {
                    peer.see(frame);
                }
                const bool forget                  = std::uniform_int_distribution<int>{0, 49}(engine) == 0;
                const ModelReceiver::Answer answer = receiver.receive(first, aloe::frames::tcp_payload(frame), forget);
                std::vector<Block> blocks;
                blocks.reserve(answer.blocks.size());
                for (const Range& range : answer.blocks) {
                    blocks.push_back(Block{.first = base + range.first, .end = base + range.end});
                }
                receive(blocks.empty() ? peer.segment({aloe::wire::TcpFlag::Ack}, {}, peer.snd_nxt(), base + answer.ack)
                                       : peer.sack(base + answer.ack, blocks));
                for (aloe::frames::ParsedFrame& sent_now : collect()) {
                    left(std::move(sent_now), step);
                }
            }
            advance(std::chrono::milliseconds{1});

            // `in_flight()` is floored at zero and capped by the flight, so the totals it is taken from must fit the
            // flight too, which they can only when no entry is marked both received and lost.
            EXPECT_LE(c->in_flight(), c->unacknowledged());
            EXPECT_LE(std::uint64_t{c->reported_bytes()} + c->lost_bytes(), c->unacknowledged());
            std::uint64_t reported = 0;
            std::uint64_t lost     = 0;
            const TcpSequence una  = c->acknowledged();
            for (std::size_t position = 0; position < c->queued_segments(); ++position) {
                const auto& entry = tcp_->sent_segment(*c, position);
                EXPECT_FALSE(entry.reported && entry.lost) << "entry " << position;
                const TcpSequence from  = una.after(entry.first) ? una : entry.first;
                const auto uncovered    = static_cast<std::uint32_t>((entry.first + entry.length) - from);
                reported               += entry.reported ? uncovered : 0U;
                lost                   += entry.lost ? uncovered : 0U;
            }
            EXPECT_EQ(c->reported_bytes(), reported) << "the sum over the entries marked received";
            EXPECT_EQ(c->lost_bytes(), lost) << "the sum over the entries marked lost";
            EXPECT_EQ(resent_reported, 0U) << "a segment left that starts an entry marked received";
            EXPECT_EQ(receiver.corrupt(), 0U) << "a byte the receiver got is not the stream's";
        }
        // The run's premise: the channel dropped, duplicated and held segments, and the receiver forgot ranges.
        EXPECT_GT(channel.dropped(), 0U);
        EXPECT_GT(channel.duplicated(), 0U);
        EXPECT_GT(channel.held(), 0U);
        EXPECT_GT(receiver.forgotten(), 0U);
        EXPECT_GT(offered, 0U);
        EXPECT_EQ(receiver.next(), offered) << "every byte written arrived in order";
        EXPECT_EQ(c->queued_segments(), 0U);
        EXPECT_EQ(c->recovery_state(), aloe::tcp::RecoveryState::Open);
        c->release();
        std::ignore = collect();  // the release's RST
    }
}
