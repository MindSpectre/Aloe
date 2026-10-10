#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include <flow_table.hpp>

namespace aloe::tcp {

    namespace {

        [[nodiscard]] std::size_t capacity_for(const std::size_t connections) {
            if (connections == 0) {
                throw std::invalid_argument{"FlowTable needs at least one connection"};
            }
            if (connections > (std::size_t{1} << 30U)) {
                throw std::invalid_argument{"FlowTable: twice the connections must fit a 32-bit index"};
            }
            return std::bit_ceil(2 * connections);
        }

    }  // namespace

    FlowTable::FlowTable(const std::size_t connections)
        : entries_(capacity_for(connections)),
          mask_{static_cast<std::uint32_t>(entries_.size() - 1)} {
    }

    std::optional<std::uint32_t> FlowTable::locate(const FlowKey key, const std::uint32_t mixed) const noexcept {
        std::uint32_t slot = home(mixed);
        for (std::size_t probed = 0; probed < entries_.size(); ++probed) {
            const Entry& entry = entries_[slot];
            if (!entry.used) {
                return std::nullopt;
            }
            if (entry.mixed == mixed && entry.key == key) {
                return slot;
            }
            slot = (slot + 1) & mask_;
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> FlowTable::find(const FlowKey key, const std::uint32_t base_hash) const noexcept {
        const std::optional<std::uint32_t> slot = locate(key, detail::mix_flow_hash(base_hash));
        if (!slot) {
            return std::nullopt;
        }
        return entries_[*slot].index;
    }

    bool FlowTable::insert(const FlowKey key, const std::uint32_t index, const std::uint32_t base_hash) noexcept {
        if (size_ == entries_.size()) {
            return false;
        }
        const std::uint32_t mixed = detail::mix_flow_hash(base_hash);
        std::uint32_t slot        = home(mixed);
        while (entries_[slot].used) {
            if (entries_[slot].mixed == mixed && entries_[slot].key == key) {
                return false;
            }
            slot = (slot + 1) & mask_;
        }
        entries_[slot] = Entry{.key = key, .index = index, .mixed = mixed, .used = true};
        ++size_;
        return true;
    }

    bool FlowTable::erase(const FlowKey key, const std::uint32_t base_hash) noexcept {
        const std::optional<std::uint32_t> found = locate(key, detail::mix_flow_hash(base_hash));
        if (!found) {
            return false;
        }
        std::uint32_t hole = *found;
        std::uint32_t next = (hole + 1) & mask_;
        // A full table has no empty slot to stop at, so the scan also stops after one lap.
        for (std::size_t scanned = 1; scanned < entries_.size() && entries_[next].used; ++scanned) {
            // An entry moves back into the hole only if the hole is not before its home on the probe path.
            const std::uint32_t its_home = home(entries_[next].mixed);
            if (distance(its_home, hole) < distance(its_home, next)) {
                entries_[hole] = entries_[next];
                hole           = next;
            }
            next = (next + 1) & mask_;
        }
        entries_[hole] = Entry{};
        --size_;
        return true;
    }

}  // namespace aloe::tcp
