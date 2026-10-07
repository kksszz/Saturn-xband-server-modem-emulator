#pragma once
#include <cstdint>
#include <limits>

namespace xband {
// Single-byte diagnostic delivery stage. Explicit guest-tick period, no host
// clock, burst catch-up, assumed UART format or automatic register configuration.
class GuestBytePacer {
public:
    explicit GuestBytePacer(uint64_t period) : period_(period), failed_(period == 0) {}
    // An already-held byte keeps its old deadline; new period applies next offer.
    bool setPeriod(uint64_t period) noexcept {
        if (failed_) return false;
        if (!period) { fail(); return false; }
        period_ = period; return true;
    }
    bool offer(uint8_t byte, uint64_t now) noexcept {
        if (!observe(now) || busy_) return false;
        if (now > std::numeric_limits<uint64_t>::max() - period_) { fail(); return false; }
        byte_ = byte; due_ = now + period_; busy_ = true; return true;
    }
    // false sink means no acceptance; retain byte, even after its deadline.
    template<class Sink> bool step(uint64_t now, Sink sink) noexcept {
        if (!observe(now) || !busy_ || now < due_) return false;
        try {
            if (!sink(byte_)) return false;
            busy_ = false; return true;
        } catch (...) { fail(); return false; }
    }
    void reset() noexcept { busy_ = false; failed_ = period_ == 0; last_ = due_ = 0; byte_ = 0; }
    bool busy() const noexcept { return busy_; }
    bool failed() const noexcept { return failed_; }
private:
    bool observe(uint64_t now) noexcept {
        if (failed_) return false;
        if (now < last_) { fail(); return false; }
        last_ = now; return true;
    }
    void fail() noexcept { failed_ = true; busy_ = false; byte_ = 0; }
    uint64_t period_, last_ = 0, due_ = 0;
    uint8_t byte_ = 0;
    bool busy_ = false, failed_ = false;
};
}
