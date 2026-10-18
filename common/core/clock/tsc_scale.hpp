#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace aloe::core {

    /**
     * @brief Converts TSC ticks to nanoseconds as `(ticks * multiplier) >> shift`, the kernel's form.
     *
     * Made from one measurement: the ticks that passed while a reference clock advanced by a known number of
     * nanoseconds. The multiplier is 32 bits and the shift the largest, up to 32, that keeps it so. The product
     * is taken in two 32-bit halves, as the kernel's `mul_u64_u32_shr` takes it, so a count converts exactly
     * (rounded down) whenever its result fits 64 bits: about 584 years of nanoseconds. The kernel's
     * `clocks_calc_mult_shift` also caps the shift by the longest interval it must convert; the split product
     * makes that cap unnecessary here.
     */
    class TscScale {
    public:
        /// The scale of `ticks` against `nanoseconds`. Throws `std::invalid_argument` for a zero count, for a tick
        /// longer than an unshifted 32-bit multiplier holds (2^32 ns, 4.3 s), and for a tick so short that the
        /// multiplier would round to zero.
        [[nodiscard]] static TscScale measured(const std::uint64_t ticks, const std::uint64_t nanoseconds) {
            if (ticks == 0 || nanoseconds == 0) {
                throw std::invalid_argument{"a TSC scale needs a nonzero count of ticks and of nanoseconds"};
            }
            constexpr double multiplier_limit = 4294967296.0;  // 2^32
            const double per_tick             = static_cast<double>(nanoseconds) / static_cast<double>(ticks);
            for (unsigned shift = 32;; --shift) {
                const double multiplier = std::round(std::ldexp(per_tick, static_cast<int>(shift)));
                if (multiplier < multiplier_limit) {
                    if (multiplier == 0.0) {
                        throw std::invalid_argument{"a TSC tick too short for a 32-bit multiplier to scale"};
                    }
                    return TscScale{static_cast<std::uint32_t>(multiplier), shift};
                }
                if (shift == 0) {
                    throw std::invalid_argument{"a TSC tick too long for a 32-bit multiplier to scale"};
                }
            }
        }

        /// `ticks` in nanoseconds, rounded down.
        [[nodiscard]] std::uint64_t nanoseconds(const std::uint64_t ticks) const noexcept {
            const std::uint64_t low  = ticks & std::uint64_t{0xFFFF'FFFF};
            const std::uint64_t high = ticks >> 32U;
            std::uint64_t result     = (low * multiplier_) >> shift_;
            if (high != 0) {
                result += (high * multiplier_) << (32U - shift_);
            }
            return result;
        }

    private:
        TscScale(const std::uint32_t multiplier, const unsigned shift) noexcept
            : multiplier_{multiplier},
              shift_{shift} {
        }

        std::uint32_t multiplier_ = 0;
        unsigned shift_           = 0;
    };

}  // namespace aloe::core
