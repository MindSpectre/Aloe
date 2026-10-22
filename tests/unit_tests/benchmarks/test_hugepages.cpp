#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <hugepages.hpp>
#include <unistd.h>

namespace {

    using namespace aloe::benchmarks;

    /// A kernel in miniature: `nr_hugepages` and `free_hugepages`, a write that may be refused or only partly honoured.
    class FakeSysfs final : public HugepageSysfs {
    public:
        std::uint64_t reserved      = 0;
        std::uint64_t free          = 0;
        bool refuse                 = false;       ///< Not root.
        std::uint64_t grant_at_most = UINT64_MAX;  ///< A fragmented machine reserves fewer than asked.
        std::vector<std::uint64_t> writes;

        [[nodiscard]] std::uint64_t read(const std::string_view file) override {
            return file == "nr_hugepages" ? reserved : free;
        }

        void write_reserved(const std::uint64_t pages) override {
            if (refuse) {
                throw std::system_error{std::make_error_code(std::errc::permission_denied), "nr_hugepages"};
            }
            writes.push_back(pages);
            const std::uint64_t granted = pages > reserved ? std::min(pages - reserved, grant_at_most) : 0;
            if (pages >= reserved) {
                reserved += granted;
                free     += granted;
            } else {
                const std::uint64_t released  = std::min(reserved - pages, free);
                reserved                     -= released;
                free                         -= released;
            }
        }
    };

}  // namespace

TEST(Hugepages, EnoughFreePagesWritesNothing) {
    FakeSysfs sysfs;
    sysfs.reserved = 128;
    sysfs.free     = 100;
    {
        const HugepageReservation reservation{sysfs, 64, false};
        EXPECT_EQ(reservation.reserved_by_us(), 0U);
        EXPECT_EQ(reservation.free_before(), 100U);
    }
    EXPECT_TRUE(sysfs.writes.empty());
}

TEST(Hugepages, TheShortfallIsReservedAndReleasedAtExit) {
    FakeSysfs sysfs;
    sysfs.reserved = 10;
    sysfs.free     = 4;
    {
        const HugepageReservation reservation{sysfs, 64, false};
        EXPECT_EQ(reservation.reserved_by_us(), 60U);
        EXPECT_EQ(sysfs.reserved, 70U);
        EXPECT_EQ(sysfs.free, 64U);
    }
    EXPECT_EQ(sysfs.reserved, 10U) << "put back to what was found";
    EXPECT_EQ(sysfs.writes, (std::vector<std::uint64_t>{70, 10}));
}

TEST(Hugepages, KeepLeavesTheReservation) {
    FakeSysfs sysfs;
    {
        const HugepageReservation reservation{sysfs, 8, true};
        EXPECT_EQ(reservation.reserved_by_us(), 8U);
    }
    EXPECT_EQ(sysfs.reserved, 8U);
}

TEST(Hugepages, AFragmentedMachineFailsWithTheShortfallAndRestores) {
    FakeSysfs sysfs;
    sysfs.grant_at_most = 32;
    try {
        const HugepageReservation reservation{sysfs, 64, false};
        FAIL() << "half the pages is a failure";
    } catch (const HugepageError& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("32 of the 64"), std::string::npos) << what;
        EXPECT_NE(what.find("compact_memory"), std::string::npos) << what;
    }
    EXPECT_EQ(sysfs.reserved, 0U) << "the partial reservation was put back";
}

TEST(Hugepages, WithoutPrivilegeTheErrorNamesBothRemedies) {
    FakeSysfs sysfs;
    sysfs.refuse = true;
    try {
        const HugepageReservation reservation{sysfs, 64, false};
        FAIL() << "no privilege, no pages";
    } catch (const HugepageError& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("sudo"), std::string::npos) << what;
        EXPECT_NE(what.find(manual_reservation_command(64)), std::string::npos) << what;
    }
}

TEST(Hugepages, FileSysfsReadsAndWritesUnderARoot) {
    // The PID keeps two runs of this binary at once, two presets say, out of each other's directory.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::format("aloe-hugepages-test-{}", getpid());
    std::filesystem::create_directories(root);
    std::ofstream{root / "nr_hugepages"} << "12\n";
    std::ofstream{root / "free_hugepages"} << "5\n";
    FileHugepageSysfs sysfs{root};
    EXPECT_EQ(sysfs.read("nr_hugepages"), 12U);
    EXPECT_EQ(sysfs.read("free_hugepages"), 5U);
    sysfs.write_reserved(40);
    EXPECT_EQ(sysfs.read("nr_hugepages"), 40U);
    std::filesystem::remove_all(root);
}
