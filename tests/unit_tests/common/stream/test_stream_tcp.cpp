#include <aloe/fabric>
#include <aloe/net>
#include <aloe/stream>
#include <aloe/tcp>

#include <gtest/gtest.h>

// The brick's connection is the stream contract, and its view is the read view: pinned where the contract lives.
static_assert(aloe::stream::IsStream<aloe::tcp::Connection<aloe::net::Ipv4<aloe::fabric::Port>>>);
static_assert(aloe::stream::IsReadView<aloe::tcp::ReadView<aloe::fabric::Packet>>);

TEST(StreamContract, TcpConnectionModelsIt) {
    SUCCEED();
}
