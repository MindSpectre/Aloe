#include <aloe/loop>
#include <vector>

#include <gtest/gtest.h>

namespace {

    /// Records its id when run, and re-pushes itself once if asked to.
    struct Recorder : aloe::loop::Work {
        Recorder(std::vector<int>& record, const int id)
            : Work{&Recorder::execute},
              record_{&record},
              id_{id} {
        }

        void requeue_once_into(aloe::loop::RunQueue& queue) noexcept {
            requeue_ = &queue;
        }

        static void execute(aloe::loop::Work& work) noexcept {
            auto& self = static_cast<Recorder&>(work);
            self.record_->push_back(self.id_);
            if (self.requeue_ != nullptr) {
                aloe::loop::RunQueue* queue = self.requeue_;
                self.requeue_               = nullptr;
                queue->push(self);
            }
        }

    private:
        std::vector<int>* record_;
        int id_;
        aloe::loop::RunQueue* requeue_ = nullptr;
    };

}  // namespace

TEST(RunQueue, RunsInPushOrderAndTakeEmptiesIt) {
    aloe::loop::RunQueue queue;
    std::vector<int> record;
    Recorder first{record, 1};
    Recorder second{record, 2};
    Recorder third{record, 3};
    EXPECT_TRUE(queue.empty());

    queue.push(first);
    queue.push(second);
    queue.push(third);
    EXPECT_FALSE(queue.empty());

    aloe::loop::Work* chain = queue.take();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(aloe::loop::RunQueue::run_chain(chain), 3);
    EXPECT_EQ(record, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(queue.take(), nullptr);
}

// Work that re-pushes itself runs on the next step, not in the same chain.
TEST(RunQueue, WorkPushedWhileAChainRunsWaitsForTheNextTake) {
    aloe::loop::RunQueue queue;
    std::vector<int> record;
    Recorder self_pusher{record, 1};
    Recorder other{record, 2};
    self_pusher.requeue_once_into(queue);

    queue.push(self_pusher);
    queue.push(other);
    EXPECT_EQ(aloe::loop::RunQueue::run_chain(queue.take()), 2);
    EXPECT_EQ(record, (std::vector<int>{1, 2})) << "the re-push did not run in the same chain";
    EXPECT_FALSE(queue.empty()) << "the re-pushed node waits in the queue";

    EXPECT_EQ(aloe::loop::RunQueue::run_chain(queue.take()), 1);
    EXPECT_EQ(record, (std::vector<int>{1, 2, 1}));
    EXPECT_TRUE(queue.empty());
}
