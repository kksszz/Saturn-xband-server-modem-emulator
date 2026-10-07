#pragma once
#include <xband/protocol_state.hpp>

namespace xband::protocol {
// Owner-thread accounting, not a socket/output queue. Commit only after the
// complete data message has been admitted to a bounded ordered transport queue.
class SendWindow {
public:
    Result commit(size_t bytes) noexcept {
        if (failed_) return Result::inactive;
        if (bytes == 0 || bytes > ReceiveWindow::max_chunk) return Result::invalid;
        if (bytes > std::numeric_limits<uint64_t>::max() - next_) return fail(Result::overflow);
        if (next_ + bytes > limit_) return Result::capacity; // Normal backpressure; retry later.
        next_ += bytes;
        return Result::ok;
    }
    Result acknowledge(uint64_t next, uint64_t limit) noexcept {
        if (failed_) return Result::inactive;
        // Receiver may advertise credit only for bytes actually consumed, which
        // cannot exceed acknowledged bytes. Subtraction avoids uint64 overflow.
        if (next < ack_ || next > next_) return fail(Result::sequence);
        if (limit < limit_ || limit < next || limit - next > ReceiveWindow::capacity)
            return fail(Result::capacity);
        ack_ = next; limit_ = limit;
        return Result::ok;
    }
    uint64_t nextOffset() const noexcept { return next_; }
    uint64_t acknowledged() const noexcept { return ack_; }
    uint64_t limit() const noexcept { return limit_; }
    bool failed() const noexcept { return failed_; }
    void reset() noexcept { next_ = ack_ = 0; limit_ = ReceiveWindow::capacity; failed_ = false; }
private:
    Result fail(Result result) noexcept { failed_ = true; return result; }
    uint64_t next_ = 0, ack_ = 0, limit_ = ReceiveWindow::capacity;
    bool failed_ = false;
};
}
