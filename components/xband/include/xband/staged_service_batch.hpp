#pragma once
#include <xband/remote_service_batch.hpp>

namespace xband {
// Owner-thread response staging, separate from the guest UART FIFO. This does
// not model baud timing. Caller drains only at an allowed guest-time boundary.
// Same borrowed ClientControl lifetime rule as RemoteServiceBatch applies.
template<size_t Capacity = 65536>
class StagedServiceBatch {
    static_assert(Capacity > 0 && Capacity <= 65536);
public:
    explicit StagedServiceBatch(protocol::ClientControl &client) : batch_(client) {}
    bool begin(std::span<const uint8_t> input, uint64_t tick) {
        if (failed_ || !batch_.begin(input, tick)) return false;
        ready_ = false; return true;
    }
    RemoteServiceBatch::Step resume(uint64_t now, size_t budget = 4096) noexcept {
        if (failed_) return RemoteServiceBatch::Step::failed;
        const auto result = batch_.resume(now, [&](uint8_t byte) {
            // Blocking here would wait for a paused guest forever. End the
            // connection instead: bounded memory, no dropped/replayed bytes.
            if (size_ == Capacity) throw std::runtime_error("remote response staging overflow");
            bytes_[(head_ + size_) % Capacity] = byte; ++size_; return true;
        }, budget);
        if (result == RemoteServiceBatch::Step::failed) { clearFailed(); return result; }
        if (result == RemoteServiceBatch::Step::done) ready_ = true;
        return result;
    }
    // A full guest FIFO is normal: keep remaining bytes and let the caller run
    // the guest after network completion, rather than awaiting FIFO space.
    template<class Sink>
    size_t drain(Sink sink, size_t budget = 256) noexcept {
        if (!readyToRun()) return 0;
        size_t delivered = 0;
        try {
            while (size_ && delivered < budget) {
                if (!sink(bytes_[head_])) break;
                head_ = (head_ + 1) % Capacity; --size_; ++delivered;
            }
        } catch (...) { cancel(); }
        return delivered;
    }
    void cancel() noexcept { batch_.cancel(); clearFailed(); }
    bool readyToRun() const noexcept { return ready_ && !failed_ && !batch_.busy(); }
    bool failed() const noexcept { return failed_; }
    size_t pending() const noexcept { return size_; }
private:
    void clearFailed() noexcept { failed_ = true; ready_ = false; head_ = size_ = 0; }
    RemoteServiceBatch batch_;
    std::array<uint8_t, Capacity> bytes_{};
    size_t head_ = 0, size_ = 0;
    bool ready_ = false, failed_ = false;
};
}
