#pragma once
#include <xband/client_control.hpp>
#include <algorithm>

namespace xband {
// Async replacement boundary for the legacy pumpService invocation. Caller
// schedules TCP separately and holds guest execution at this batch boundary.
// Client outlives this object. No sleeps, sockets, guessed frame frequency or
// ownership of emulator objects; guest_tick must be supplied by the adapter.
class RemoteServiceBatch {
public:
    enum class Step { idle, progress, blocked, done, failed };
    explicit RemoteServiceBatch(protocol::ClientControl &client) : client_(client) {}
    bool begin(std::span<const uint8_t> bytes, uint64_t guest_tick) {
        if (phase_ != Phase::idle || client_.state() != protocol::ClientControl::State::service ||
            client_.advancing() || !client_.received().empty() || bytes.size() > input_.size() ||
            guest_tick < client_.completedTick()) return false;
        call_ = client_.call();
        std::copy(bytes.begin(), bytes.end(), input_.begin());
        size_ = bytes.size(); cursor_ = 0; tick_ = guest_tick; phase_ = Phase::sending;
        return true;
    }
    // sink must return true only after accepting a byte, false with no effects.
    // Exceptions are fatal, since uncertain guest effects must never be replayed.
    template<class Sink>
    Step resume(uint64_t host_now, Sink sink, size_t budget = 4096) noexcept {
        if (phase_ == Phase::failed) return Step::failed;
        if (phase_ == Phase::idle) return Step::idle;
        try {
            if (!client_.poll(host_now) || client_.state() != protocol::ClientControl::State::service || client_.call() != call_) return fail();
            if (!budget) return Step::blocked;
            if (phase_ == Phase::sending) {
                if (cursor_ < size_) {
                    const size_t count = std::min({size_ - cursor_, budget, protocol::ReceiveWindow::max_chunk});
                    const auto result = client_.transmit(std::span(input_).subspan(cursor_, count), host_now);
                    if (result == protocol::Admission::queue_full || result == protocol::Admission::credit_full) return Step::blocked;
                    if (result != protocol::Admission::queued) return fail();
                    cursor_ += count; budget -= count;
                    if (cursor_ < size_ || !budget) return Step::progress;
                }
                const auto result = client_.advance(tick_, host_now);
                if (result == protocol::Admission::queue_full) return Step::blocked;
                if (result != protocol::Admission::queued) return fail();
                phase_ = Phase::waiting;
            }
            while (budget && !client_.received().empty()) {
                const auto bytes = client_.received();
                const size_t count = std::min(bytes.size(), budget);
                size_t accepted = 0;
                while (accepted < count && sink(bytes[accepted])) ++accepted;
                if (accepted && !client_.consumeReceived(accepted, host_now)) return fail();
                budget -= accepted;
                if (accepted < count) return Step::blocked;
            }
            if (client_.advancing() || !client_.received().empty()) return Step::blocked;
            if (client_.completedTick() != tick_) return fail();
            phase_ = Phase::idle; size_ = cursor_ = 0; call_.clear(); return Step::done;
        } catch (...) { return fail(); }
    }
    // Cancellation ends the connection, not just a local cursor. It cannot
    // retract bytes already queued/accepted by the remote service.
    void cancel() noexcept { fail(); }
    bool busy() const noexcept { return phase_ == Phase::sending || phase_ == Phase::waiting; }
private:
    enum class Phase { idle, sending, waiting, failed };
    Step fail() noexcept {
        client_.disconnect(); phase_ = Phase::failed; size_ = cursor_ = 0; call_.clear(); return Step::failed;
    }
    protocol::ClientControl &client_; Phase phase_ = Phase::idle;
    std::array<uint8_t, protocol::ReceiveWindow::capacity> input_{};
    size_t size_ = 0, cursor_ = 0; uint64_t tick_ = 0; std::string call_;
};
}
