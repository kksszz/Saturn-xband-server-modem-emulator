#pragma once
#include <xband/frame_codec.hpp>

namespace xband {
// Owner-thread, bounded FIFO of complete framed messages. Opaque payloads;
// schema, per-call credit and message class policy belong above this layer.
class FrameQueue {
public:
    static constexpr size_t byte_capacity = 2 * (FrameDecoder::max_payload + 4);
    static constexpr size_t message_capacity = 128;
    enum class Result { ok, full, invalid };
    Result enqueue(std::span<const uint8_t> payload) noexcept {
        if (payload.empty() || payload.size() > FrameDecoder::max_payload) return Result::invalid;
        const size_t length = payload.size() + 4;
        if (messages_ == message_capacity || length > byte_capacity - used_) return Result::full;
        const auto n = static_cast<uint32_t>(payload.size());
        append(static_cast<uint8_t>(n >> 24)); append(static_cast<uint8_t>(n >> 16));
        append(static_cast<uint8_t>(n >> 8)); append(static_cast<uint8_t>(n));
        for (auto byte : payload) append(byte);
        remaining_[(first_ + messages_) % message_capacity] = length;
        ++messages_; return Result::ok;
    }
    // Borrowed contiguous bytes, potentially covering multiple messages. Send
    // this span once and consume only the positive count reported by the socket.
    // A would-block result consumes nothing. No retained views across mutation.
    std::span<const uint8_t> peek() const noexcept {
        const size_t count = used_ < byte_capacity - head_ ? used_ : byte_capacity - head_;
        return {bytes_.data() + head_, count};
    }
    bool consume(size_t count) noexcept {
        if (count > peek().size()) return false;
        head_ = (head_ + count) % byte_capacity; used_ -= count;
        while (count != 0) {
            auto &left = remaining_[first_];
            if (count < left) { left -= count; break; }
            count -= left; left = 0; first_ = (first_ + 1) % message_capacity; --messages_;
        }
        return true;
    }
    size_t pendingBytes() const noexcept { return used_; }
    size_t pendingMessages() const noexcept { return messages_; }
    // Call on disconnect with decoder reset. Old asynchronous socket completions
    // MUST be rejected by the host generation check before calling consume().
    void reset() noexcept {
        bytes_.fill(0); remaining_.fill(0); head_ = used_ = first_ = messages_ = 0;
    }
private:
    void append(uint8_t byte) noexcept { bytes_[(head_ + used_) % byte_capacity] = byte; ++used_; }
    std::array<uint8_t, byte_capacity> bytes_{};
    std::array<size_t, message_capacity> remaining_{};
    size_t head_ = 0, used_ = 0, first_ = 0, messages_ = 0;
};
}
