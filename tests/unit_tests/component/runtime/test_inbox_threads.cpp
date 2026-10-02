#include <aloe/runtime>
#include <cstddef>
#include <deque>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    constexpr std::size_t producers          = 6;
    constexpr std::size_t nodes_per_producer = 2000;

    struct Message : aloe::runtime::Work {
        Message(const std::size_t producer, const std::size_t sequence)
            : producer_{producer},
              sequence_{sequence} {
        }

        std::size_t producer_;
        std::size_t sequence_;
    };

}  // namespace

// The test the tsan preset exists for: every producer pushes from its own thread while the
// consumer pops, and every node arrives exactly once, in its producer's order.
TEST(InboxThreads, NodesFromManyThreadsArriveExactlyOnceInProducerOrder) {
    aloe::runtime::Inbox inbox;
    std::vector<std::deque<Message>> storage(producers);  // deque: Message is immovable
    for (std::size_t producer = 0; producer < producers; ++producer) {
        for (std::size_t sequence = 0; sequence < nodes_per_producer; ++sequence) {
            storage[producer].emplace_back(producer, sequence);
        }
    }

    std::set<std::pair<std::size_t, std::size_t>> seen;
    std::vector<std::size_t> next_expected(producers, 0);
    std::size_t received = 0;

    std::jthread consumer{[&] {
        while (received < producers * nodes_per_producer) {
            aloe::runtime::Work* work = inbox.pop();
            if (work == nullptr) {
                std::this_thread::yield();
                continue;
            }
            auto& message = static_cast<Message&>(*work);
            EXPECT_TRUE(seen.emplace(message.producer_, message.sequence_).second) << "a node arrived twice";
            EXPECT_EQ(message.sequence_, next_expected[message.producer_]) << "out of order within a producer";
            next_expected[message.producer_] = message.sequence_ + 1;
            ++received;
        }
    }};

    {
        std::vector<std::jthread> threads;
        threads.reserve(producers);
        for (std::size_t producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&inbox, &storage, producer] {
                for (Message& message : storage[producer]) {
                    inbox.push(message);
                }
            });
        }
    }
    consumer.join();

    EXPECT_EQ(received, producers * nodes_per_producer);
    EXPECT_TRUE(inbox.empty());
}
