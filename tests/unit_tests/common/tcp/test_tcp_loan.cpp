#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <aloe/wire>
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
    EXPECT_EQ(tcp_->send_nodes_available(), tcp_->config().send_pool);
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
    EXPECT_EQ(c->delivered(), 400U) << "the front the copy left out counts as delivered, as a trim counts it";
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
