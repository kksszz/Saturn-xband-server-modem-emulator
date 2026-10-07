#pragma once
#include <xband/service_endpoint.hpp>
#include <xband/protocol_state.hpp>
#include <numeric>

namespace xband {
// Explicit rational legacy frames per guest tick. No assumed 60 Hz, wall clock
// or milliseconds. Ratio is supplied by the emulator adapter for the whole call.
class FrameClock {
public:
    FrameClock(uint64_t numerator, uint64_t denominator) noexcept {
        if (numerator == 0 || denominator == 0) return;
        const auto divisor = std::gcd(numerator, denominator);
        numerator_ = numerator / divisor; denominator_ = denominator / divisor;
        valid_ = numerator_ <= 1000000 && denominator_ <= 1000000000000ULL;
    }
    bool map(uint64_t tick, unsigned &frame) const noexcept {
        if (!valid_) return false;
        const auto whole = tick / denominator_, remainder = tick % denominator_;
        const uint64_t maximum = std::numeric_limits<unsigned>::max();
        if (whole > maximum / numerator_) return false;
        const auto base = whole * numerator_;
        const auto fraction = remainder * numerator_ / denominator_; // bounded by ratio limits
        if (fraction > maximum - base) return false;
        frame = static_cast<unsigned>(base + fraction); return true;
    }
private:
    uint64_t numerator_ = 0, denominator_ = 1;
    bool valid_ = false;
};

// Call-local staged input -> service -> output pump. Host authenticates call IDs,
// handles wire ACKs and emits advance_ok only after done. Service must be fresh
// when this object is constructed; discard it and the service state on hangup.
class TimedService {
public:
    enum class Step { idle, progress, blocked, done, failed };
    TimedService(ServiceEndpoint &service, FrameClock clock) noexcept : service_(service), clock_(clock) {}
    protocol::Result accept(uint64_t offset, std::span<const uint8_t> bytes) noexcept {
        if (failed_) return protocol::Result::inactive;
        if (phase_ != Phase::idle) return protocol::Result::capacity; // caller retains data
        const auto result = input_.accept(offset, bytes);
        if (result != protocol::Result::ok) fail();
        return result;
    }
    bool beginAdvance(uint64_t tick) noexcept {
        if (failed_ || phase_ != Phase::idle) return false;
        unsigned frame = 0;
        if (tick < last_tick_ || !clock_.map(tick, frame)) { fail(); return false; }
        tick_ = tick; frame_ = frame; phase_ = Phase::input; return true;
    }
    // Bounded work per call, no busy-wait. sink returns true only after an output
    // byte has been safely admitted downstream. False means no side effects.
    // Sink/service exceptions are fatal; no automatic replay of uncertain effects.
    template<class Sink>
    Step resume(Sink sink, size_t budget = 4096) noexcept {
        if (failed_) return Step::failed;
        if (phase_ == Phase::idle) return Step::idle;
        try {
            while (budget != 0) {
                if (phase_ == Phase::input) {
                    const auto pending = input_.peek();
                    if (!pending.empty()) {
                        if (!service_.transmit(pending.front(), frame_)) return Step::blocked;
                        if (input_.consume(1) != protocol::Result::ok) return fail();
                        --budget; continue;
                    }
                    phase_ = Phase::timer;
                }
                if (phase_ == Phase::timer) {
                    service_.tick(frame_); phase_ = Phase::output; --budget; continue;
                }
                uint8_t byte = 0;
                if (!service_.peek(byte)) {
                    phase_ = Phase::idle; last_tick_ = tick_; return Step::done;
                }
                if (!sink(byte)) return Step::blocked;
                service_.consume(); --budget;
            }
            return Step::progress;
        } catch (...) { return fail(); }
    }
    uint64_t receivedOffset() const noexcept { return input_.nextOffset(); }
    uint64_t receiveLimit() const noexcept { return input_.limit(); }
    size_t pendingInput() const noexcept { return input_.pending(); }
    uint64_t completedTick() const noexcept { return last_tick_; }
    bool failed() const noexcept { return failed_; }
private:
    enum class Phase { idle, input, timer, output };
    Step fail() noexcept { failed_ = true; input_.reset(); return Step::failed; }
    ServiceEndpoint &service_;
    FrameClock clock_;
    protocol::ReceiveWindow input_;
    uint64_t tick_ = 0, last_tick_ = 0;
    unsigned frame_ = 0;
    Phase phase_ = Phase::idle;
    bool failed_ = false;
};
}
