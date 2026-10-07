#pragma once
#include <cstdint>
#include <limits>
#include <numeric>

namespace xband {
// Owner-thread gate for a scheduler-cycle callback. Protocol ticks are fixed
// nanoseconds, derived from an exact rational cycle frequency (not GUI FPS).
// A changed frequency or timeline revision requires a new connection/gate.
class GuestExecutionGate {
public:
    static constexpr uint64_t clock_hz = 1000000000;
    // Diagnostic safety bound, not a hardware maximum instruction duration.
    static constexpr uint64_t quantum = 4096, max_overrun = 64;
    GuestExecutionGate(uint64_t frequency_num, uint64_t frequency_den)
        : frequency_num_(frequency_num), frequency_den_(frequency_den) {
        if (!frequency_num || !frequency_den || frequency_den > UINT64_MAX / clock_hz) { failed_ = true; return; }
        multiplier_ = frequency_den * clock_hz; divisor_ = frequency_num;
        const auto gcd = std::gcd(multiplier_, divisor_); multiplier_ /= gcd; divisor_ /= gcd;
    }
    uint64_t budget(uint64_t cycle, uint64_t revision, uint64_t frequency_num, uint64_t frequency_den) noexcept {
        if (failed_) return 0;
        if (frequency_num != frequency_num_ || frequency_den != frequency_den_) return fail();
        if (!initialized_) {
            initialized_ = true; origin_ = observed_ = limit_ = cycle; revision_ = revision; waiting_ = true; return 0;
        }
        if (revision != revision_ || cycle < observed_ || (cycle > limit_ && cycle - limit_ > max_overrun)) return fail();
        // While held, even an instruction-sized unexplained advance is invalid.
        if (waiting_ && cycle != observed_) return fail();
        observed_ = cycle;
        const uint64_t elapsed = cycle - origin_, whole = elapsed / divisor_, remainder = elapsed % divisor_;
        if (whole > UINT64_MAX / multiplier_ || (remainder && multiplier_ > UINT64_MAX / remainder)) return fail();
        const auto base = whole * multiplier_, fraction = remainder * multiplier_ / divisor_;
        if (fraction > UINT64_MAX - base) return fail();
        tick_ = base + fraction;
        waiting_ = cycle >= limit_;
        return waiting_ ? 0 : limit_ - cycle;
    }
    // Release one quantum only after the remote batch at the current boundary
    // has completed. The caller, not this gate, owns that completion decision.
    bool release() noexcept {
        if (failed_ || !initialized_ || !waiting_ || observed_ > UINT64_MAX - quantum) return false;
        limit_ = observed_ + quantum; waiting_ = false; return true;
    }
    bool failed() const noexcept { return failed_; }
    bool waiting() const noexcept { return waiting_; }
    uint64_t tick() const noexcept { return tick_; }
    uint64_t cycle() const noexcept { return observed_; }
private:
    uint64_t fail() noexcept { failed_ = true; waiting_ = true; return 0; }
    uint64_t frequency_num_, frequency_den_, multiplier_ = 1, divisor_ = 1;
    uint64_t origin_ = 0, observed_ = 0, limit_ = 0, revision_ = 0, tick_ = 0;
    bool initialized_ = false, waiting_ = false, failed_ = false;
};
}
