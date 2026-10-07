#pragma once
#include <xband/send_window.hpp>

namespace xband::protocol {
// Client-side service call lifecycle AFTER authenticated session establishment.
// Schema, envelope IDs/session, subscriber, target and server-issued token
// uniqueness must be checked outside this layer. This is not peer dial/answer.
class ServiceCall {
public:
    enum class State { idle, opening, service, closing, error };
    State state() const noexcept { return state_; }
    std::string_view call() const noexcept {
        return has_call_ ? std::string_view(call_.data(), call_.size()) : std::string_view{};
    }
    Result beginOpen(uint64_t request) noexcept {
        if (state_ != State::idle) return Result::inactive;
        if (request == 0) return Result::invalid;
        pending_ = request; state_ = State::opening; return Result::ok;
    }
    Result opened(uint64_t reply_to, std::string_view id) noexcept {
        if (state_ != State::opening || reply_to != pending_ || !token(id)) return fail(Result::invalid);
        for (size_t i = 0; i < call_.size(); ++i) call_[i] = id[i];
        has_call_ = true; pending_ = 0; state_ = State::service; return Result::ok;
    }
    Result beginClose(uint64_t request) noexcept {
        if (state_ != State::service) return Result::inactive;
        if (request == 0) return Result::invalid;
        sender_.reset(); receiver_.reset(); // No stale guest bytes after hangup.
        pending_ = request; state_ = State::closing; return Result::ok;
    }
    Result closed(uint64_t reply_to, std::string_view id) noexcept {
        if (state_ != State::closing || reply_to != pending_ || id != call()) return fail(Result::invalid);
        disconnect(); return Result::ok;
    }
    Result receive(std::string_view id, uint64_t offset, std::span<const uint8_t> bytes) noexcept {
        if (state_ != State::service) return Result::inactive;
        if (id != call()) return fail(Result::invalid);
        const auto result = receiver_.accept(offset, bytes);
        return result == Result::ok ? result : fail(result);
    }
    Result sent(size_t bytes) noexcept {
        if (state_ != State::service) return Result::inactive;
        const auto result = sender_.commit(bytes);
        return sender_.failed() ? fail(result) : result;
    }
    Result acknowledged(std::string_view id, uint64_t next, uint64_t limit) noexcept {
        if (state_ != State::service) return Result::inactive;
        if (id != call()) return fail(Result::invalid);
        const auto result = sender_.acknowledge(next, limit);
        return result == Result::ok ? result : fail(result);
    }
    std::span<const uint8_t> peek() const noexcept {
        return state_ == State::service ? receiver_.peek() : std::span<const uint8_t>{};
    }
    Result consume(size_t count) noexcept {
        if (state_ != State::service) return Result::inactive;
        const auto result = receiver_.consume(count);
        return result == Result::ok ? result : fail(result);
    }
    uint64_t sendOffset() const noexcept { return sender_.nextOffset(); }
    uint64_t receiveOffset() const noexcept { return receiver_.nextOffset(); }
    uint64_t receiveLimit() const noexcept { return receiver_.limit(); }
    // Called on transport loss or explicit session teardown. Adapter must also
    // discard queued wire frames, pending timers and borrowed views.
    void disconnect() noexcept {
        sender_.reset(); receiver_.reset(); call_.fill(0);
        has_call_ = false; pending_ = 0; state_ = State::idle;
    }
    // Adapter uses abort on timeout/server error; no automatic replay/reopen.
    void abort() noexcept { (void)fail(Result::inactive); }
private:
    Result fail(Result result) noexcept { disconnect(); state_ = State::error; return result; }
    SendWindow sender_;
    ReceiveWindow receiver_;
    std::array<char, 32> call_{};
    uint64_t pending_ = 0;
    bool has_call_ = false;
    State state_ = State::idle;
};
}
