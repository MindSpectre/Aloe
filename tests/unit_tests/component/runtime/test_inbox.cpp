#include <aloe/runtime>
#include <vector>

#include <gtest/gtest.h>

namespace {

    struct Numbered : aloe::runtime::Work {
        explicit Numbered(const int id)
            : id_{id} {
        }

        [[nodiscard]] int id() const noexcept {
            return id_;
        }

    private:
        int id_;
    };

    std::vector<int> drain(aloe::runtime::Inbox& inbox) {
        std::vector<int> ids;
        for (aloe::runtime::Work* work = inbox.pop(); work != nullptr; work = inbox.pop()) {
            ids.push_back(static_cast<Numbered&>(*work).id());
        }
        return ids;
    }

}  // namespace

TEST(Inbox, PopsInPushOrderAndReportsEmpty) {
    aloe::runtime::Inbox inbox;
    EXPECT_TRUE(inbox.empty());
    EXPECT_EQ(inbox.pop(), nullptr);

    Numbered a{1};
    Numbered b{2};
    Numbered c{3};
    inbox.push(a);
    EXPECT_FALSE(inbox.empty());
    inbox.push(b);
    inbox.push(c);

    EXPECT_EQ(drain(inbox), (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(inbox.empty());
    EXPECT_EQ(inbox.pop(), nullptr);
}

TEST(Inbox, InterleavedPushAndPopKeepsOrder) {
    aloe::runtime::Inbox inbox;
    Numbered a{1};
    Numbered b{2};
    Numbered c{3};
    inbox.push(a);
    inbox.push(b);
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 1);
    inbox.push(c);
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 2);
    EXPECT_FALSE(inbox.empty());
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 3);
    EXPECT_TRUE(inbox.empty());

    // A node may be pushed again once it has been popped.
    inbox.push(a);
    EXPECT_EQ(static_cast<Numbered*>(inbox.pop())->id(), 1);
    EXPECT_TRUE(inbox.empty());
}
