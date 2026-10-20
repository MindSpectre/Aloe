#include "hugepages.hpp"

#include <cerrno>
#include <cstddef>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace aloe::benchmarks {

    FileHugepageSysfs::FileHugepageSysfs(std::filesystem::path root)
        : root_{std::move(root)} {
    }

    std::uint64_t FileHugepageSysfs::read(const std::string_view file) {
        const std::filesystem::path path = root_ / file;
        std::ifstream stream{path};
        if (!stream) {
            throw std::system_error{errno, std::generic_category(), path.string()};
        }
        std::uint64_t value = 0;
        stream >> value;
        return value;
    }

    void FileHugepageSysfs::write_reserved(const std::uint64_t pages) {
        const std::string path = (root_ / "nr_hugepages").string();
        const int fd           = ::open(path.c_str(), O_WRONLY);
        if (fd < 0) {
            throw std::system_error{errno, std::generic_category(), path};
        }
        const std::string text = std::to_string(pages);
        const ssize_t written  = ::write(fd, text.data(), text.size());
        const int error        = errno;
        ::close(fd);
        if (written < 0 || static_cast<std::size_t>(written) != text.size()) {
            throw std::system_error{written < 0 ? error : EIO, std::generic_category(), path};
        }
    }

    std::string manual_reservation_command(const std::uint64_t pages) {
        return std::format("echo {} | sudo tee {}", pages, (hugepage_root / "nr_hugepages").string());
    }

    HugepageReservation::HugepageReservation(HugepageSysfs& sysfs, const std::uint64_t needed, const bool keep)
        : sysfs_{&sysfs},
          keep_{keep} {
        free_before_ = sysfs.read("free_hugepages");
        original_    = sysfs.read("nr_hugepages");
        raised_to_   = original_;
        if (free_before_ >= needed) {
            return;
        }
        const std::uint64_t shortfall = needed - free_before_;
        try {
            sysfs.write_reserved(original_ + shortfall);
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::permission_denied) {
                throw HugepageError{std::format(
                    "{} free 2 MB hugepages, {} needed, and only root may reserve them. Either run the benchmark "
                    "under sudo, so it reserves {} more for its own run and releases them after, or reserve them by "
                    "hand first: {}",
                    free_before_,
                    needed,
                    shortfall,
                    manual_reservation_command(original_ + shortfall))};
            }
            throw;
        }
        raised_to_                 = sysfs.read("nr_hugepages");
        const std::uint64_t gained = raised_to_ > original_ ? raised_to_ - original_ : 0;
        if (free_before_ + gained < needed) {
            try {
                sysfs.write_reserved(original_);
            } catch (const std::system_error&) {  // NOLINT(bugprone-empty-catch): best effort on the way out
            }
            raised_to_ = original_;
            throw HugepageError{
                std::format("the kernel reserved {} of the {} hugepages asked for: memory is fragmented. Try "
                            "`echo 1 | sudo tee /proc/sys/vm/compact_memory` and run again, or run with --hugepages {}",
                            gained,
                            shortfall,
                            free_before_ + gained)};
        }
    }

    HugepageReservation::~HugepageReservation() {
        if (keep_ || raised_to_ == original_) {
            return;
        }
        try {
            sysfs_->write_reserved(original_);
        } catch (const std::system_error&) {  // NOLINT(bugprone-empty-catch): a destructor reports nothing
        }
    }

}  // namespace aloe::benchmarks
