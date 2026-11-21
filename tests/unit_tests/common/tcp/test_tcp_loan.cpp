#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstdint>
#include <tuple>

#include <gtest/gtest.h>
#include <tcp_fixture.hpp>

// The busy rule: a resend that finds its packet still lent asks the device to reclaim, defers to the next flush up to
// `retransmit_defers` times, and then sends one counted copy of the unacknowledged part as a second lent packet,
// reconciled with the original when either comes back. The Held port keeps every packet until `return_loans`.
namespace {

    using namespace std::chrono_literals;
    using TcpLoan       = aloe::testing::TcpHeldFixture;
    using TcpLoanOnStep = aloe::testing::TcpFixture;

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpLoan,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpLoanOnStep,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpLoan, AResendThatFindsThePacketLentIsDeferredAndGoesOnceReturned) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    ASSERT_EQ(collect().size(), 1U);
    EXPECT_EQ(loans_outstanding(), 2U) << "the SYN-ACK and the data segment: Held returns nothing on its own";
    advance(200ms);
    EXPECT_TRUE(collect().empty()) << "deferred: the packet is still lent";
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U) << "counted as a timer-driven resend";
    EXPECT_EQ(c->rto(), 400ms) << "backoff proceeds as if it had left";
    return_loans();
    const auto resent = collect();  // the flush retries the deferred resend
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(aloe::frames::tcp_payload(resent[0]).size(), 100U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 0U);
    return_loans();
}

TEST_P(TcpLoan, AFastRetransmitThatFindsThePacketLentIsCountedOnceAndGoesOnceReturned) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);  // three segments
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 3U);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery());
    EXPECT_TRUE(collect().empty()) << "deferred: the head's packet is still lent";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    EXPECT_EQ(c->lost_bytes(), 1460U) << "marked lost while it waits";
    return_loans();
    const auto resent = collect();  // the flush runs the pass again
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U) << "a retry of a waiting entry counts nothing";
    EXPECT_EQ(tcp_->counters().retransmits_copied, 0U);
    EXPECT_EQ(c->lost_bytes(), 0U) << "the resend that left cleared the mark";
    return_loans();
}

TEST_P(TcpLoan, NothingNewIsOfferedWhileTheHeadsResendWaitsAfterATimeout) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1460)), 1460U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 1U);
    advance(200ms);  // the RTO marks the head; its packet is still lent
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(c->congestion_window(), 1460U);
    EXPECT_EQ(c->in_flight(), 0U);
    EXPECT_EQ(c->lost_bytes(), 1460U);
    EXPECT_EQ(c->writable(), 0U) << "the window's one segment is the head's";
    EXPECT_EQ(c->send(aloe::frames::pattern(1460)), 0U) << "new data would take the window and starve the resend";
    advance(1ms);
    const auto head = collect();  // the flush: the bound reached, the copy goes
    ASSERT_EQ(head.size(), 1U);
    EXPECT_EQ(head[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().retransmits, 1U) << "no second expiry was needed";
    EXPECT_EQ(c->lost_bytes(), 0U);
    return_loans();
}

TEST_P(TcpLoan, NothingNewIsOfferedWhileTheHeadsResendWaitsInRecovery) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(14600)), 14600U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 10U);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(c->in_recovery());
    EXPECT_TRUE(collect().empty()) << "the head's resend waits for its packet";
    EXPECT_EQ(c->in_flight(), 8760U);
    EXPECT_EQ(c->congestion_window(), 9490U) << "730 above what is in flight, for the head";
    EXPECT_EQ(c->writable(), 0U) << "an entry that owes a resend goes before anything new";
    EXPECT_EQ(c->send(aloe::frames::pattern(730)), 0U);
    advance(1ms);
    const auto head = collect();  // the flush: the copy goes, on the window it was allowed
    ASSERT_EQ(head.size(), 1U);
    EXPECT_EQ(head[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(c->in_flight(), 10220U);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    return_loans();
}

TEST_P(TcpLoan, WritableIsRaisedWhenTheFlushSendsTheLastEntryThatOwedAResend) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2920)), 2920U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 2U);
    // Drains the pending events and says how many of them carried Writable.
    const auto writables = [&] {
        int count = 0;
        for (auto event = poll(); event; event = poll()) {
            count += event->events.writable() ? 1 : 0;
        }
        return count;
    };
    advance(200ms);  // the RTO marks both; the head's resend waits for its packet
    EXPECT_EQ(c->writable(), 0U);
    EXPECT_EQ(c->send(aloe::frames::pattern(1460)), 0U) << "a writer is refused here, and waits for Writable";
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U) << "the head's copy; the second entry still owes its resend";
    receive(peer_.ack(sent[0]));  // slow start to two segments; the second entry's first try waits for its packet
    EXPECT_EQ(c->writable(), 0U);
    EXPECT_EQ(writables(), 0) << "nothing rose: an entry still owes a resend";
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U) << "its copy, at the flush: nothing owes a resend any more";
    EXPECT_EQ(c->writable(), 1460U) << "one segment in flight in a window of two";
    process_pending();  // a rise outside an ACK is published at the next process, as a retry hint is
    EXPECT_EQ(writables(), 1) << "writable() rose at the flush";
    process_pending();
    EXPECT_EQ(writables(), 0) << "once";
    EXPECT_EQ(c->send(aloe::frames::pattern(1460)), 1460U) << "the writer that was refused can write";
    return_loans();
}

TEST_P(TcpLoan, AFlushThatLeavesMarkedEntriesBehindRaisesNoWritable) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    ASSERT_EQ(collect().size(), 3U);
    advance(200ms);  // the RTO marks all three; the head waits
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U) << "the head's copy";
    EXPECT_EQ(c->lost_bytes(), 2920U) << "two entries still owe a resend";
    EXPECT_EQ(c->writable(), 0U);
    process_pending();
    EXPECT_FALSE(poll().has_value()) << "writable() did not rise: no event";
    return_loans();
}

TEST_P(TcpLoan, WritableIsRaisedWhenAnExpirysResendClearsTheLastLostMark) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    ASSERT_EQ(collect().size(), 1U);
    advance(200ms);  // the first expiry marks the segment; its resend waits, and no flush follows
    EXPECT_EQ(c->lost_bytes(), 100U);
    EXPECT_EQ(c->writable(), 0U);
    EXPECT_EQ(c->send(aloe::frames::pattern(100)), 0U);
    EXPECT_FALSE(poll().has_value());
    advance(400ms);  // the second expiry: the bound is reached and the copy goes from the timer itself
    EXPECT_EQ(tcp_->counters().retransmits, 2U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    EXPECT_EQ(c->writable(), 1360U) << "a window of one segment, 100 bytes of it in flight";
    process_pending();
    const auto event = poll();
    ASSERT_TRUE(event.has_value()) << "writable() rose at the expiry";
    EXPECT_TRUE(event->events.writable());
    EXPECT_FALSE(poll().has_value());
    ASSERT_EQ(collect().size(), 1U);
    return_loans();
}

TEST_P(TcpLoan, AWaitingHeadThatLeavesAtTheFlushRestartsTheRetransmissionTimer) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->rto(), 200ms);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();  // t0: the timer is armed for t0 + 200 ms
    ASSERT_EQ(sent.size(), 3U);
    advance(150ms);
    const auto dup = [&] { return peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), c->acknowledged()); };
    receive(dup());
    receive(dup());
    receive(dup());
    EXPECT_TRUE(collect().empty()) << "the head's resend waits for its packet";
    advance(40ms);
    return_loans();
    const auto resent = collect();  // t0 + 190 ms: the head leaves at the flush, a retry of the waiting entry
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, sent[0].tcp->sequence);
    advance(10ms);  // t0 + 200 ms, where the first send had armed the timer
    EXPECT_TRUE(collect().empty()) << "armed again when the head left: its ACK has had no time to return";
    EXPECT_EQ(tcp_->counters().retransmits, 0U);
    advance(189ms);
    EXPECT_EQ(tcp_->counters().retransmits, 0U);
    advance(1ms);  // t0 + 390 ms: one RTO after the resend
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    return_loans();
}

TEST_P(TcpLoan, AtTheRtoAReportedHeadWhosePacketIsLentWaitsThenLeavesAtTheFlush) {
    peer_.set_sack_permitted(true);
    // A round trip of 10 ms, so RACK's reordering window is 2.5 ms and the block marks nothing at once; at a round trip
    // of zero the window is zero, and RACK would mark and resend the first entry before the ACK frees it.
    auto* c = open_passive(peer_, 10ms);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 2U);
    const aloe::wire::TcpSequence first = sent[0].tcp->sequence;
    const std::array blocks{
        aloe::wire::TcpSackBlock{.first = first + 100U, .end = first + 200U}
    };
    receive(peer_.sack(first, blocks));
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), first + 100U));
    ASSERT_TRUE(tcp_->sent_segment(*c, 0).reported) << "the head is the entry the peer reported, then discarded";
    advance(200ms);  // reneging armed the RTO at max(srtt / 2, 10 ms): it fires 10 ms on
    EXPECT_TRUE(collect().empty()) << "deferred: the head's packet is still lent";
    EXPECT_EQ(tcp_->counters().sack_reneging, 1U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    EXPECT_FALSE(tcp_->sent_segment(*c, 0).reported);
    EXPECT_EQ(c->reported_bytes(), 0U);
    return_loans();
    const auto resent = collect();  // the flush's pass finds the head marked lost
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, first + 100U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 0U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    return_loans();
}

TEST_P(TcpLoan, AfterATimeoutTheEntriesBehindTheHeadEachWaitForTheirPacketAndAreCountedOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);  // three segments, all lent from here on
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 3U);
    advance(200ms);  // the RTO marks all three; the head's resend finds its packet lent
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    advance(1ms);
    const auto head = collect();  // the bound reached: the head goes as a copy, and fills the window of one segment
    ASSERT_EQ(head.size(), 1U);
    EXPECT_EQ(head[0].tcp->sequence, sent[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 0U) << "the expiry's own resend";
    receive(peer_.ack(sent[0]));  // slow start: two segments; the second entry's first attempt finds its packet lent
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 2U);
    advance(1ms);
    const auto second = collect();  // its copy at the flush; the third entry's first attempt waits in turn
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].tcp->sequence, sent[1].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 3U);
    return_loans();
    const auto third = collect();  // in place, once its packet is back
    ASSERT_EQ(third.size(), 1U);
    EXPECT_EQ(third[0].tcp->sequence, sent[2].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U) << "each marked entry once, its retry at the flush not again";
    EXPECT_EQ(tcp_->counters().retransmits, 1U) << "one expiry";
    EXPECT_EQ(tcp_->counters().retransmits_copied, 2U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    return_loans();
}

TEST_P(TcpLoan, AWaitingEntryABlockThenReportsIsNotResentAndTheNextEntryIsTriedAfresh) {
    peer_.set_sack_permitted(true);
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    for (int segment = 0; segment < 3; ++segment) {
        ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    }
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 3U);
    const aloe::wire::TcpSequence first = sent[0].tcp->sequence;
    // With SACK the tail probe of the third goes first, two round trips of zero after the sends, the handshake's: at
    // the next tick. Its packet is lent, so it waits, pending, and the next flush sends its copy: the probe leaves at
    // 2 ms, and the RTO runs from then, 2 + 200 = 202 ms.
    advance(1ms);
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    EXPECT_EQ(tcp_->counters().tail_probes, 0U);
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U) << "the probe's copy";
    EXPECT_EQ(tcp_->counters().tail_probes, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    advance(200ms);  // the RTO at 202 ms marks all three; the head's resend waits
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U) << "the head's copy; the second entry's first attempt waits for its packet";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 3U) << "the probe's, the head's and the second's";
    const std::array blocks{
        aloe::wire::TcpSackBlock{.first = first + 100U, .end = first + 200U}
    };
    ASSERT_TRUE(tcp_->sent_segment(*c, 1).waiting);
    receive(peer_.sack(first, blocks));  // the peer holds the entry that waits
    EXPECT_TRUE(tcp_->sent_segment(*c, 1).reported);
    EXPECT_FALSE(tcp_->sent_segment(*c, 1).lost);
    EXPECT_FALSE(tcp_->sent_segment(*c, 1).waiting) << "its resend is moot: nothing of it waits";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U)
        << "the third entry's own first attempt, counted: the second's wait ended with the report";
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 4U) << "its original and its copy are both lent";
    EXPECT_TRUE(tcp_->sent_segment(*c, 2).waiting);
    return_loans();
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U) << "the third entry; the one reported received is not resent";
    EXPECT_EQ(resent[0].tcp->sequence, first + 200U);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    EXPECT_EQ(c->reported_bytes(), 100U);
    EXPECT_FALSE(tcp_->sent_segment(*c, 2).waiting);
    advance(1ms);
    EXPECT_TRUE(collect().empty()) << "nothing waits any more";
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 4U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 2U) << "the probe's and the head's";
    return_loans();
}

TEST_P(TcpLoan, AnAckThatFreesTheHeadLeavesALaterWaitingEntryToTheFlush) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(4380)), 4380U);
    const auto sent = collect();
    ASSERT_EQ(sent.size(), 3U);
    advance(200ms);
    EXPECT_TRUE(collect().empty());
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U) << "the head's copy";
    // An ACK inside the head grows the window past what is left of it, so the second entry's turn comes while the
    // head is still queued: its packet is lent, and it waits.
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), sent[0].tcp->sequence + 500U));
    EXPECT_TRUE(collect().empty());
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 2U);
    receive(peer_.ack(sent[0]));  // frees the head while the second entry waits
    EXPECT_EQ(c->queued_segments(), 2U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 0).waiting) << "the entry that waited is the head now, and waits still";
    EXPECT_TRUE(collect().empty()) << "still the flush's, once its packet is back: no copy before its time";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 1U) << "not a second first attempt";
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    return_loans();
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 2U) << "the one that waited, in place, and the third behind it";
    EXPECT_EQ(resent[0].tcp->sequence, sent[1].tcp->sequence);
    EXPECT_EQ(resent[1].tcp->sequence, sent[2].tcp->sequence);
    EXPECT_EQ(tcp_->counters().fast_retransmits, 2U);
    EXPECT_EQ(tcp_->counters().retransmits, 1U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    EXPECT_EQ(c->lost_bytes(), 0U);
    return_loans();
}

TEST_P(TcpLoan, ADuplicateSynWhileTheSynAckIsLentGetsItOnceReturned) {
    ASSERT_TRUE(tcp_->listen(aloe::testing::tcp_listen_port).has_value());
    receive(peer_.syn());
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    aloe::frames::TcpPeer again{aloe::testing::peer_spec(), aloe::wire::TcpSequence{aloe::testing::tcp_peer_isn}};
    receive(again.syn());
    EXPECT_TRUE(collect().empty()) << "deferred: the SYN-ACK's packet is still lent";
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    return_loans();
    const auto resent = collect();  // the flush retries the deferred resend
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_TRUE(aloe::testing::is_flags(resent[0], {aloe::wire::TcpFlag::Syn, aloe::wire::TcpFlag::Ack}));
    EXPECT_EQ(resent[0].tcp->sequence, first[0].tcp->sequence);
    EXPECT_EQ(tcp_->counters().retransmits, 0U) << "an answer to the peer, not a timer's resend";
    EXPECT_EQ(tcp_->counters().fast_retransmits, 0U);
    return_loans();
}

TEST_P(TcpLoan, AfterTheBoundACopyIsSentAndBothPacketsAreFreedOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(100)), 100U);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    advance(200ms);
    EXPECT_TRUE(collect().empty()) << "deferral one of one";
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    advance(1ms);
    const auto copied = collect();  // the next flush: still lent, the bound reached, the copy goes
    ASSERT_EQ(copied.size(), 1U);
    EXPECT_EQ(copied[0].tcp->sequence, first[0].tcp->sequence);
    EXPECT_TRUE(std::ranges::equal(aloe::frames::tcp_payload(copied[0]), aloe::frames::tcp_payload(first[0])));
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    EXPECT_FALSE(tcp_->sent_segment(*c, 0).copy.empty());
    EXPECT_EQ(loans_outstanding(), 3U);
    receive(peer_.ack(copied[0]));  // acknowledged while both are lent: freed on return
    EXPECT_EQ(c->queued_segments(), 0U);
    return_loans();
    EXPECT_EQ(loans_outstanding(), 0U);
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool - 1) << "less the FIN's reserved node";
}

TEST_P(TcpLoan, TheCopyCarriesOnlyTheUnacknowledgedPart) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1000)), 1000U);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), first[0].tcp->sequence + 400U));
    advance(200ms);
    advance(1ms);
    const auto copied = collect();
    ASSERT_EQ(copied.size(), 1U);
    EXPECT_EQ(copied[0].tcp->sequence, first[0].tcp->sequence + 400U);
    EXPECT_EQ(aloe::frames::tcp_payload(copied[0]).size(), 600U);
    EXPECT_TRUE(
        std::ranges::equal(aloe::frames::tcp_payload(copied[0]), aloe::frames::tcp_payload(first[0]).subspan(400)))
        << "the bytes past the acknowledged front";
    return_loans();
}

TEST_P(TcpLoan, RetransmitDefersZeroCopiesAtOnce) {
    configure_tcp({.retransmit_defers = 0});
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    EXPECT_EQ(collect().size(), 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 0U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    return_loans();
}

TEST_P(TcpLoan, WithACopyOutAndBothLentTheNextResendDefersAgain) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    advance(1ms);
    EXPECT_EQ(collect().size(), 1U) << "the copy";
    advance(400ms);
    EXPECT_TRUE(collect().empty()) << "no third packet";
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 2U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    return_loans();
}

TEST_P(TcpLoan, WhenTheOriginalReturnsFirstTheCopyIsDropped) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    advance(1ms);
    std::ignore = collect();  // the copy is out
    return_loans();           // both back
    advance(400ms);           // the next resend reconciles: one packet, in place
    EXPECT_EQ(collect().size(), 1U);
    EXPECT_TRUE(tcp_->sent_segment(*c, 0).copy.empty());
    EXPECT_EQ(tcp_->counters().retransmits_copied, 1U);
    return_loans();
}

TEST_P(TcpLoan, WhenTheCopyReturnsFirstItTakesTheOriginalsPlace) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(1000)), 1000U);
    const auto first = collect();
    ASSERT_EQ(first.size(), 1U);
    receive(peer_.segment({aloe::wire::TcpFlag::Ack}, {}, peer_.snd_nxt(), first[0].tcp->sequence + 400U));
    advance(200ms);
    advance(1ms);
    tcp_->flush(now_);  // the copy goes into the shard queue's ring
    queue_.discard();   // and its handle is dropped untransmitted: the copy is back, the original still lent
    advance(400ms);     // the next resend reconciles: the copy takes the original's place and leaves in place
    const auto resent = collect();
    ASSERT_EQ(resent.size(), 1U);
    EXPECT_EQ(resent[0].tcp->sequence, first[0].tcp->sequence + 400U);
    EXPECT_TRUE(
        std::ranges::equal(aloe::frames::tcp_payload(resent[0]), aloe::frames::tcp_payload(first[0]).subspan(400)));
    const auto& entry = tcp_->sent_segment(*c, 0);
    EXPECT_TRUE(entry.copy.empty());
    EXPECT_EQ(entry.first, first[0].tcp->sequence + 400U);
    EXPECT_EQ(entry.payload, 600U);
    EXPECT_EQ(entry.length, 600U);
    EXPECT_EQ(c->delivered(), 400U) << "the front the copy left out, counted once by the ACK that covered it";
    receive(peer_.ack(resent[0]));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(c->delivered(), 1000U);
    return_loans();
    EXPECT_EQ(loans_outstanding(), 0U);
}

TEST_P(TcpLoan, AnAckWhileLentFreesThePacketOnReturn) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U);
    receive(peer_.ack(frames[0]));
    EXPECT_EQ(c->queued_segments(), 0U);
    EXPECT_EQ(loans_outstanding(), 2U) << "the SYN-ACK's and the segment's handles are the device's still";
    return_loans();
    EXPECT_EQ(loans_outstanding(), 0U);
}

TEST_P(TcpLoan, AbortWithLoansOutLeaksNothing) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(2000)), 2000U);
    std::ignore = collect();
    c->abort();
    EXPECT_EQ(collect().size(), 1U) << "the RST";
    c->release();
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool);
    return_loans();
    EXPECT_EQ(loans_outstanding(), 0U);
}

TEST_P(TcpLoan, ReleaseWhileDeferredLeavesNoDanglingEntry) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 1U);
    c->release();
    const auto frames = collect();
    ASSERT_EQ(frames.size(), 1U) << "release aborts with a RST; the flush walks the deferred list and finds nothing";
    EXPECT_TRUE(aloe::testing::is_flags(frames[0], {aloe::wire::TcpFlag::Rst, aloe::wire::TcpFlag::Ack}));
    EXPECT_EQ(tcp_->table_size(), 0U);
    return_loans();
    EXPECT_EQ(loans_outstanding(), 0U);
}

TEST_P(TcpLoan, ReleaseWithACopyInFlightFreesBothPacketsOnce) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore = collect();
    advance(200ms);
    advance(1ms);
    ASSERT_EQ(collect().size(), 1U);
    c->release();
    return_loans();
    EXPECT_EQ(loans_outstanding(), 0U);
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool);
    // The fixture's destruction asserts every block of the fabric's pool came back exactly once.
}

TEST_P(TcpLoanOnStep, ReclaimFreesAFinishedLoanBeforeTheResendDefers) {
    auto* c = open_passive(peer_);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->send(aloe::frames::pattern(10)), 10U);
    std::ignore  = collect();
    now_        += 200ms;
    fabric_->advance(now_);  // finished, not yet released: no queue call ran
    EXPECT_TRUE(tcp_->sent_segment(*c, 0).packet.lent());
    std::ignore = wheel_.advance(now_);  // the RTO: lent, reclaim releases it, resent in place
    EXPECT_EQ(collect().size(), 1U);
    EXPECT_EQ(tcp_->counters().retransmits_deferred, 0U);
    EXPECT_EQ(tcp_->counters().retransmits_copied, 0U);
}
