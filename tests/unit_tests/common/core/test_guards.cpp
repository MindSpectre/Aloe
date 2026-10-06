#include <aloe/core>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

namespace {

    /// A handle like the ethdev packet: a const member could still write through the pointer, and
    /// the guards are what keeps the mutating members honestly non-const.
    class Handle {
    public:
        explicit Handle(int& target) noexcept
            : target_{&target} {
        }

        void set(const int value) noexcept {
            aloe::core::force_non_const(this);
            aloe::core::force_non_static(this);
            *target_ = value;
        }

        [[nodiscard]] int get() const noexcept {
            aloe::core::force_non_static(this);
            return *target_;
        }

    private:
        int* target_;
    };

    // The guards accept exactly what `this` is in the members they are written for.
    static_assert(noexcept(aloe::core::force_non_const(std::declval<Handle*>())));
    static_assert(noexcept(aloe::core::force_non_static(std::declval<const Handle*>())));
    static_assert(std::is_void_v<decltype(aloe::core::force_non_const(std::declval<Handle*>()))>);

}  // namespace

TEST(Guards, DoNothingAtRunTimeInsideTheMembersTheyPin) {
    int target = 0;
    Handle handle{target};
    handle.set(42);
    EXPECT_EQ(handle.get(), 42);
    EXPECT_EQ(target, 42);
}
