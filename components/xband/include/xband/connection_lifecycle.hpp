#pragma once
#include <xband/protocol_state.hpp>

namespace xband::protocol {
// Trusted-LAN plaintext TCP policy, not a credential verifier. No TLS phase.
// Owner-thread only. now is host monotonic milliseconds, NEVER guest ticks.
// Every callback carries the generation returned by begin(), including timers.
class ConnectionLifecycle {
public:
    enum class State { closed, hello_wait, verifying, active };
    enum class Failure { none, timeout, clock_reversed, authentication, protocol, exhausted };
    static constexpr uint64_t handshake_ms = 5000, request_ms = 5000, idle_ms = 10000, ping_ms = 2000;

    uint64_t begin(uint64_t now) noexcept {
        close();
        if (generation_ == std::numeric_limits<uint64_t>::max()) { failure_ = Failure::exhausted; return 0; }
        ++generation_; start_ = last_clock_ = last_rx_ = last_ping_ = now;
        failure_ = Failure::none; state_ = State::hello_wait; return generation_;
    }
    // Call only for a fully validated initial hello (id=1, empty session).
    // Copies endpoint; no key is retained. Verifier runs outside this class.
    bool hello(uint64_t generation, std::string_view name, uint64_t now) noexcept {
        if (!update(generation, now)) return false;
        if (state_ != State::hello_wait || !endpoint(name)) return reject(Failure::protocol);
        for (size_t i = 0; i < name.size(); ++i) endpoint_[i] = name[i];
        endpoint_size_ = name.size(); state_ = State::verifying; return true;
    }
    // Host reports key/endpoint verification AND successful endpoint reservation.
    // The key travels in plaintext: this is not protection against LAN attackers.
    // Session must be freshly securely generated externally; this checks syntax only.
    bool authenticationCompleted(uint64_t generation, bool approved, std::string_view session, uint64_t now) noexcept {
        if (!update(generation, now)) return false;
        if (state_ != State::verifying) return reject(Failure::protocol);
        if (!approved) return reject(Failure::authentication);
        if (!guard_.bind(std::string_view(endpoint_.data(), endpoint_size_), session, 2)) return reject(Failure::protocol);
        state_ = State::active; last_rx_ = last_ping_ = now; return true;
    }
    // Full schema and allowed inbound direction must be checked before this
    // commit. Subsequent call-level rejection must be handled before game effects.
    // Bad IDs/session never refresh the liveness timer.
    bool received(uint64_t generation, unsigned version, std::string_view name,
                  std::string_view session, std::string_view id, uint64_t now) noexcept {
        if (!update(generation, now)) return false;
        if (state_ != State::active || guard_.accept(version, name, session, id) != Result::ok)
            return reject(Failure::protocol);
        last_rx_ = now; return true;
    }
    bool poll(uint64_t generation, uint64_t now) noexcept { return update(generation, now); }
    // Read-only observation at last processed host time; does not send/advance.
    bool pingDue() const noexcept { return state_ == State::active && last_clock_ - last_ping_ >= ping_ms && last_clock_ - last_rx_ >= ping_ms; }
    bool pingQueued(uint64_t generation, uint64_t now) noexcept {
        if (!update(generation, now) || state_ != State::active) return false;
        last_ping_ = now; return true;
    }
    // Single outstanding open/close/advance request slot. Arm only once an ordered
    // output message is queued. A busy/invalid local request does not reset timers.
    bool requestQueued(uint64_t generation, uint64_t id, uint64_t now) noexcept {
        if (!update(generation, now) || state_ != State::active || request_ != 0 || id == 0) return false;
        request_ = id; request_start_ = now; return true;
    }
    // Called after received() and expected response-type/body validation. Unrelated
    // traffic/pong must never complete this slot or extend its deadline.
    bool requestCompleted(uint64_t generation, uint64_t reply_to, uint64_t now) noexcept {
        if (!update(generation, now)) return false;
        if (state_ != State::active || request_ == 0 || request_ != reply_to) return reject(Failure::protocol);
        request_ = 0; return true;
    }
    void close() noexcept {
        state_ = State::closed; guard_.reset(); endpoint_.fill(0); endpoint_size_ = 0; request_ = 0;
    }
    State state() const noexcept { return state_; }
    Failure failure() const noexcept { return failure_; }
    uint64_t generation() const noexcept { return generation_; }
private:
    bool reject(Failure reason) noexcept { close(); failure_ = reason; return false; }
    bool update(uint64_t generation, uint64_t now) noexcept {
        // A late callback for an old connection must not mutate the new one.
        if (generation != generation_ || state_ == State::closed) return false;
        if (now < last_clock_) return reject(Failure::clock_reversed);
        last_clock_ = now;
        if (state_ != State::active) {
            if (now - start_ >= handshake_ms) return reject(Failure::timeout);
        } else if (now - last_rx_ >= idle_ms || (request_ != 0 && now - request_start_ >= request_ms))
            return reject(Failure::timeout);
        return true;
    }
    SessionGuard guard_;
    std::array<char, 32> endpoint_{};
    size_t endpoint_size_ = 0;
    uint64_t generation_ = 0, start_ = 0, last_clock_ = 0, last_rx_ = 0, last_ping_ = 0;
    uint64_t request_ = 0, request_start_ = 0;
    State state_ = State::closed;
    Failure failure_ = Failure::none;
};
}
