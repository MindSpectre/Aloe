#include <aloe/tcp>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <tuple>

#include <gtest/gtest.h>

// The receive window on its own: the budget, the shift, the offer with its silly-window threshold and rounding, the
// restatement and the doubling rule on `consume`.
namespace {

    using aloe::tcp::detail::ReceiveWindow;
    using aloe::wire::TcpSequence;

    const TcpSequence start{1000};

    /// An ordinary ACK with a slot free: what it states, recorded as if it left.
    ReceiveWindow::Offer ack(ReceiveWindow& window, const TcpSequence next, const std::size_t unread) {
        const ReceiveWindow::Offer offer = window.offer(next, unread, true);
        std::ignore                      = window.record(offer);
        return offer;
    }

    /// What a segment of the kind `kind` draws states now: an ordinary offer, a restatement, or, while RCV.NXT has not
    /// moved since the window opened, the handshake's segment sent again.
    [[nodiscard]] ReceiveWindow::Offer drawn(const ReceiveWindow& window,
                                             const int kind,
                                             const TcpSequence next,
                                             const std::size_t unread,
                                             const bool slot_free,
                                             const bool handshaking) {
        switch (kind) {
            case 0:
                return window.offer(next, unread, slot_free);
            case 1:
                return window.restate(next);
            default:
                return handshaking ? window.handshake(next) : window.restate(next);
        }
    }

    /// 100000 random steps of arrivals, consumes, offers, restatements and handshake segments, in rounds that each open
    /// the window afresh; the edge never moves left and never stands before RCV.NXT.
    void never_moves_left(const std::uint32_t budget, const std::optional<std::uint8_t> scale) {
        std::mt19937 engine{42};  // NOLINT(cert-msc51-cpp): a fixed seed makes a failure reproducible
        std::uniform_int_distribution<int> action{0, 3};
        std::uniform_int_distribution<int> coin{0, 1};
        std::uniform_int_distribution<int> kind{0, 2};
        ReceiveWindow window;
        TcpSequence next{0xfff00000U};  // the run crosses the top of the sequence space
        std::size_t unread = 0;
        bool handshaking   = true;  // RCV.NXT has not moved since the window opened
        for (int step = 0; step < 100000; ++step) {
            if (step % 100 == 0) {
                window.open(next, budget, 1448, scale);
                handshaking = true;
            }
            switch (action(engine)) {
                case 0: {
                    std::uniform_int_distribution<std::uint32_t> arrive{0, window.window(next)};
                    const std::uint32_t length  = arrive(engine);
                    next                        = next + length;
                    unread                     += length;
                    handshaking                 = handshaking && length == 0;
                    break;
                }
                case 1: {
                    std::uniform_int_distribution<std::size_t> consume{0, unread};
                    unread -= consume(engine);
                    break;
                }
                default: {
                    const TcpSequence previous = window.edge();
                    const bool slot_free       = coin(engine) == 1;
                    const ReceiveWindow::Offer offer =
                        drawn(window, kind(engine), next, unread, slot_free, handshaking);
                    std::ignore = window.record(offer);
                    ASSERT_FALSE(window.edge().before(previous)) << "step " << step;
                    ASSERT_FALSE(window.edge().before(next)) << "step " << step;
                    break;
                }
            }
        }
    }

}  // namespace

TEST(ReceiveWindow, TheBudgetIsHalfOfWhatTheSlotsHold) {
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 0, 3), 370688U);
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1460, 0, 3), 373760U);
    EXPECT_EQ(ReceiveWindow::budget_of(32, 1448, 0, 0), 23168U);
}

TEST(ReceiveWindow, AConfiguredWindowIsCappedAtWhatTheSlotsHold) {
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 100000, 3), 100000U);
    EXPECT_EQ(ReceiveWindow::budget_of(4, 1448, 100000, 3), 5792U);
}

TEST(ReceiveWindow, AnUnscaledBudgetIsAtMostTheField) {
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 0, 0), 65535U);
    EXPECT_EQ(ReceiveWindow::budget_of(512, 1448, 0, 14), 370688U);
}

TEST(ReceiveWindow, TheShiftIsTheSmallestThatStatesTheBudget) {
    EXPECT_EQ(ReceiveWindow::shift_for(65535), 0);
    EXPECT_EQ(ReceiveWindow::shift_for(65536), 1);
    EXPECT_EQ(ReceiveWindow::shift_for(373760), 3);
    EXPECT_EQ(ReceiveWindow::shift_for(65535U << 14U), 14);
    EXPECT_EQ(ReceiveWindow::shift_for(0xffffffffU), 14);
}

TEST(ReceiveWindow, TheHandshakeStatesTheBudgetUnscaledAndTheFirstAckScaled) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    EXPECT_EQ(window.handshake(start).field, 65535);
    EXPECT_EQ(window.edge(), start + 65535U);
    const auto first = window.offer(start, 0, true);
    EXPECT_EQ(first.field, 46336);
    EXPECT_EQ(first.edge, start + 370688U);
}

TEST(ReceiveWindow, AHandshakeSegmentSentAgainNeverMovesTheEdgeLeft) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    const auto restated = window.restate(start);
    EXPECT_EQ(restated.field, 8192) << "65535 rounded up to the unit of 8";
    std::ignore = window.record(restated);
    EXPECT_EQ(window.edge(), start + 65536U);
    const auto again = window.handshake(start);
    EXPECT_EQ(again.field, 65535) << "the handshake's field stays unscaled";
    EXPECT_EQ(again.edge, start + 65536U) << "the edge already recorded, one byte past the field's";
    EXPECT_FALSE(window.record(again));
    EXPECT_EQ(window.edge(), start + 65536U);
}

TEST(ReceiveWindow, TheEdgeMovesOnlyByAFullSegment) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    EXPECT_EQ(ack(window, start + 1000U, 1000).field, 64535) << "the edge stands; the window is what is left";
    EXPECT_EQ(ack(window, start + 1000U, 0).field, 64535) << "1000 bytes read: less than a full segment";
    EXPECT_EQ(ack(window, start + 1448U, 0).field, 65535) << "1448 beyond the edge: it moves";
}

TEST(ReceiveWindow, NoFreeSlotAndARestateBothLeaveTheEdge) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    std::ignore = ack(window, start + 3000U, 3000);
    EXPECT_EQ(window.offer(start + 3000U, 0, false).field, 62535);
    EXPECT_EQ(window.restate(start + 3000U).field, 62535);
    EXPECT_EQ(window.offer(start + 3000U, 0, true).field, 65535);
}

TEST(ReceiveWindow, TheFieldIsRoundedUpAndTheReconstructedEdgeRecorded) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    std::ignore      = ack(window, start, 0);              // the edge is start + 370688
    const auto offer = window.offer(start + 3U, 3, true);  // 370685 bytes to the edge: 46335.6 units
    EXPECT_EQ(offer.field, 46336);
    EXPECT_EQ(offer.edge, start + 370691U) << "what the peer reconstructs, three bytes past the computed edge";
}

TEST(ReceiveWindow, SegmentsShorterThanTheUnitNeverCloseTheWindow) {
    ReceiveWindow window;
    window.open(start, 370688, 1448, std::uint8_t{3});
    std::ignore = ack(window, start, 0);
    for (std::uint32_t received = 4; received <= 4000; received += 4) {
        EXPECT_EQ(ack(window, start + received, received).field, 46336);
    }
    EXPECT_EQ(window.edge(), start + 370688U + 4000U)
        << "each field rounded up and recorded moves the edge on: the cap, not the budget, bounds memory";
}

TEST(ReceiveWindow, AClosedWindowStaysClosedUntilAFullSegmentFits) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    std::ignore = ack(window, start + 65535U, 65535);
    EXPECT_EQ(window.window(start + 65535U), 0U);
    EXPECT_EQ(window.offer(start + 65535U, 65535 - 1447, true).field, 0);
    EXPECT_FALSE(window.update_due(start + 65535U, 65535 - 1447, true));
    EXPECT_TRUE(window.update_due(start + 65535U, 65535 - 1448, true));
    EXPECT_EQ(window.offer(start + 65535U, 65535 - 1448, true).field, 1448);
}

TEST(ReceiveWindow, AConsumeOwesAnUpdateOnlyWhenTheWindowDoubles) {
    ReceiveWindow window;
    window.open(start, 65535, 1448, std::nullopt);
    std::ignore = ack(window, start + 40000U, 40000);  // 25535 on offer
    EXPECT_FALSE(window.update_due(start + 40000U, 20000, true)) << "45535 is less than twice 25535";
    EXPECT_TRUE(window.update_due(start + 40000U, 10000, true)) << "55535 is more";
    EXPECT_FALSE(window.update_due(start + 40000U, 10000, false)) << "no slot: the edge may not move";
}

TEST(ReceiveWindow, WrapsAcrossTheTopOfTheSequenceSpace) {
    ReceiveWindow window;
    const TcpSequence high{0xffffff00U};
    window.open(high, 65535, 1448, std::nullopt);
    EXPECT_TRUE(window.edge().after(high));
    EXPECT_EQ(window.window(high), 65535U);
    EXPECT_EQ(ack(window, high + 0x200U, 0).field, 65535 - 0x200) << "past the wrap, 512 bytes on: the edge stands";
    EXPECT_EQ(ack(window, high + 0x600U, 0).edge, high + 0x600U + 65535U) << "1536 bytes on: it moves";
    EXPECT_EQ(window.window(high + 0x600U), 65535U);
}

TEST(ReceiveWindow, TheEdgeNeverMovesLeftOverARandomRun) {
    never_moves_left(370688, std::uint8_t{3});
    never_moves_left(65535, std::nullopt);
}
