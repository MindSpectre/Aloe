#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aloe::benchmarks {

    /// Where Linux keeps the 2 MB hugepage counts.
    inline const std::filesystem::path hugepage_root{"/sys/kernel/mm/hugepages/hugepages-2048kB"};

    /// The run cannot have the hugepages it needs; `what()` is the message for the user, remedy included.
    class HugepageError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /// The two files a reservation reads and the one it writes, behind an interface so a test needs no root.
    class HugepageSysfs {
    public:
        HugepageSysfs()                                = default;
        HugepageSysfs(const HugepageSysfs&)            = delete;
        HugepageSysfs& operator=(const HugepageSysfs&) = delete;
        HugepageSysfs(HugepageSysfs&&)                 = delete;
        HugepageSysfs& operator=(HugepageSysfs&&)      = delete;
        virtual ~HugepageSysfs()                       = default;

        /// `nr_hugepages` or `free_hugepages`. Throws std::system_error when the file cannot be read.
        [[nodiscard]] virtual std::uint64_t read(std::string_view file) = 0;
        /// Writes `nr_hugepages`. Throws std::system_error, `permission_denied` when the process is not root.
        virtual void write_reserved(std::uint64_t pages)                = 0;
    };

    /// The files under `root`.
    class FileHugepageSysfs final : public HugepageSysfs {
    public:
        explicit FileHugepageSysfs(std::filesystem::path root = hugepage_root);
        [[nodiscard]] std::uint64_t read(std::string_view file) override;
        void write_reserved(std::uint64_t pages) override;

    private:
        std::filesystem::path root_;
    };

    /// `echo <pages> | sudo tee <root>/nr_hugepages`: the manual remedy, for the error message and the docs.
    [[nodiscard]] std::string manual_reservation_command(std::uint64_t pages);

    /**
     * @brief The run's hugepages, reserved by this process when it can and released when it is done.
     *
     * Construction reads the free pages. Enough: nothing happens. Not enough and the write succeeds: the reservation
     * rises by the shortfall and is read back, and a kernel that granted less, fragmented, gets its reservation put
     * back and the caller a HugepageError with the shortfall and the remedy. Not enough and the write is refused: a
     * HugepageError with both remedies, the benchmark under sudo or the reservation by hand. Destruction writes the
     * original count back unless `keep`, which frees only pages nobody uses.
     */
    class HugepageReservation {
    public:
        HugepageReservation(HugepageSysfs& sysfs, std::uint64_t needed, bool keep);
        HugepageReservation(const HugepageReservation&)            = delete;
        HugepageReservation& operator=(const HugepageReservation&) = delete;
        HugepageReservation(HugepageReservation&&)                 = delete;
        HugepageReservation& operator=(HugepageReservation&&)      = delete;
        ~HugepageReservation();

        [[nodiscard]] std::uint64_t reserved_by_us() const noexcept {
            return raised_to_ - original_;
        }

        [[nodiscard]] std::uint64_t free_before() const noexcept {
            return free_before_;
        }

    private:
        HugepageSysfs* sysfs_;
        std::uint64_t original_    = 0;
        std::uint64_t raised_to_   = 0;
        std::uint64_t free_before_ = 0;
        bool keep_;
    };

}  // namespace aloe::benchmarks
