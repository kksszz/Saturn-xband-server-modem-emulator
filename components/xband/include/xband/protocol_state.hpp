#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace xband::protocol {
// Typed validation only: callers must first validate the complete JSON schema.
inline bool decimal(std::string_view value, uint64_t &out) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
    uint64_t result = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<uint64_t>(c - '0');
        if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    out = result;
    return true;
}
inline bool token(std::string_view value) noexcept {
    if (value.size() != 32) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool endpoint(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32) return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
enum class Result { ok, inactive, invalid, stale_session, sequence, capacity, overflow };

// Post-authentication inbound envelope guard, not a handshake/authenticator.
// One owner thread and one instance per direction. A rejection is sticky until
// a new authenticated session is bound. Transport adapter must close on failure.
class SessionGuard {
public:
    bool bind(std::string_view name, std::string_view session, uint64_t first_id) noexcept {
        reset();
        if (!endpoint(name) || !token(session) || first_id == 0) return false;
        for (size_t i = 0; i < name.size(); ++i) endpoint_[i] = name[i];
        for (size_t i = 0; i < session.size(); ++i) session_[i] = session[i];
        length_ = name.size(); next_ = first_id; active_ = true;
        return true;
    }
    Result accept(unsigned version, std::string_view name, std::string_view session,
                  std::string_view id) noexcept {
        if (!active_) return Result::inactive;
        uint64_t number = 0;
        Result result = Result::ok;
        if (version != 2 || name != std::string_view(endpoint_.data(), length_) ||
            !decimal(id, number) || number == 0) result = Result::invalid;
        else if (session != std::string_view(session_.data(), session_.size())) result = Result::stale_session;
        else if (exhausted_ || number != next_) result = Result::sequence;
        if (result != Result::ok) { active_ = false; return result; }
        if (next_ == std::numeric_limits<uint64_t>::max()) exhausted_ = true;
        else ++next_;
        return Result::ok;
    }
    void reset() noexcept {
        endpoint_.fill(0); session_.fill(0); length_ = 0; next_ = 0;
        active_ = false; exhausted_ = false;
    }
private:
    std::array<char, 32> endpoint_{};
    std::array<char, 32> session_{};
    size_t length_ = 0;
    uint64_t next_ = 0;
    bool active_ = false, exhausted_ = false;
};

// One call/direction. Acceptance copies decoded bytes into bounded RAM before
// advertising next_offset. Credit grows only when the guest consumes bytes.
// Session/call identity and Base64/schema validation belong before this layer.
class ReceiveWindow {
public:
    static constexpr size_t capacity = 65536;
    static constexpr size_t max_chunk = 4096;
    Result accept(uint64_t offset, std::span<const uint8_t> bytes) noexcept {
        if (failed_) return Result::inactive;
        if (bytes.empty() || bytes.size() > max_chunk) return fail(Result::invalid);
        if (offset != next_) return fail(Result::sequence);
        if (bytes.size() > std::numeric_limits<uint64_t>::max() - next_) return fail(Result::overflow);
        if (bytes.size() > capacity - used_) return fail(Result::capacity);
        for (size_t i = 0; i < bytes.size(); ++i) buffer_[(head_ + used_ + i) % capacity] = bytes[i];
        used_ += bytes.size(); next_ += bytes.size();
        return Result::ok;
    }
    std::span<const uint8_t> peek() const noexcept {
        if (failed_) return {};
        const size_t contiguous = used_ < capacity - head_ ? used_ : capacity - head_;
        return {buffer_.data() + head_, contiguous};
    }
    Result consume(size_t count) noexcept {
        if (failed_) return Result::inactive;
        if (count > used_) return fail(Result::invalid);
        if (count > std::numeric_limits<uint64_t>::max() - limit_) return fail(Result::overflow);
        head_ = (head_ + count) % capacity; used_ -= count; limit_ += count;
        return Result::ok;
    }
    uint64_t nextOffset() const noexcept { return next_; }
    uint64_t limit() const noexcept { return limit_; }
    size_t pending() const noexcept { return used_; }
    bool failed() const noexcept { return failed_; }
    void reset() noexcept { buffer_.fill(0); head_ = used_ = 0; next_ = 0; limit_ = capacity; failed_ = false; }
private:
    Result fail(Result result) noexcept { failed_ = true; return result; }
    std::array<uint8_t, capacity> buffer_{};
    size_t head_ = 0, used_ = 0;
    uint64_t next_ = 0, limit_ = capacity;
    bool failed_ = false;
};
}
