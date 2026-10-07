#pragma once
#include <xband/server_handshake.hpp>
#include <xband/service_connection.hpp>
#include <functional>

namespace xband::protocol {
// Owner-thread orchestration from TCP acceptance through service disposal.
// Socket lifetime and OS-generated fresh session/call tokens remain external.
// Registry outlives this object. Factories must not reenter and must return
// independently owned service state; clock mapping is chosen by the host.
class ServerSession {
public:
    using Factory = std::function<std::unique_ptr<ServiceEndpoint>(uint64_t clock_hz)>;
    ServerSession(EndpointRegistry &registry, uint64_t now, std::string fresh_session,
                  FrameClock clock, Factory factory)
        : generation_(connection_.begin(now)), handshake_(connection_, generation_, registry),
          session_(std::move(fresh_session)), clock_(clock), factory_(std::move(factory)) {
        if (!token(session_) || !factory_) disconnect();
    }
    ServerSession(const ServerSession &) = delete;
    ServerSession &operator=(const ServerSession &) = delete;
    ~ServerSession() { disconnect(); }
    MessageInbox::FeedResult feed(std::span<const uint8_t> bytes, uint64_t now, std::string_view fresh_call = {}) noexcept {
        if (!poll(now, fresh_call)) return {0, MessageInbox::Status::failed};
        auto result = service_ ? service_->feed(bytes, now) : handshake_.feed(bytes, now, session_);
        if (result.status == MessageInbox::Status::failed) disconnect();
        return result;
    }
    bool poll(uint64_t now, std::string_view fresh_call = {}) noexcept {
        if (stopped_) return false;
        try {
            if (!handshake_.poll(now)) { disconnect(); return false; }
            if (handshake_.state() == ServerHandshake::State::accepted && !service_) {
                service_ = std::make_unique<ServiceConnection>(connection_, generation_,
                    std::string(handshake_.name()), std::string(handshake_.session()),
                    factory_(handshake_.clockHz()), clock_);
            }
            if (service_) {
                const auto result = service_->poll(now, fresh_call);
                if (result == ServiceConnection::Outcome::failed) { disconnect(); return false; }
                local_work_ = result == ServiceConnection::Outcome::handled || result == ServiceConnection::Outcome::progress;
            }
            return true;
        } catch (...) { disconnect(); return false; }
    }
    std::span<const uint8_t> output() const noexcept {
        if (stopped_) return {};
        return service_ ? service_->output() : handshake_.output();
    }
    bool sent(size_t count, uint64_t now) noexcept {
        if (stopped_ || !handshake_.poll(now)) { disconnect(); return false; }
        const bool ok = service_ ? service_->sent(count) : handshake_.sent(count, now);
        if (!ok) disconnect();
        // Factory is deferred until poll/feed; never switches output ownership
        // partway through accounting for the bytes of hello_ok.
        return ok;
    }
    bool active() const noexcept { return !stopped_ && service_ != nullptr; }
    std::string_view endpointName() const noexcept { return active() ? handshake_.name() : std::string_view{}; }
    const nlohmann::json *snapshot() const noexcept { return active() ? service_->snapshot() : nullptr; }
    std::string_view call() const noexcept { return active() ? service_->call() : std::string_view{}; }
    bool stopped() const noexcept { return stopped_; }
    // Socket readiness cannot wake work left by a consumed advance/ACK.
    bool needsPoll() const noexcept { return !stopped_ && local_work_; }
    void disconnect() noexcept {
        service_.reset(); handshake_.disconnect(); stopped_ = true; local_work_ = false; session_.clear();
    }
private:
    ConnectionLifecycle connection_; uint64_t generation_;
    ServerHandshake handshake_; std::string session_; FrameClock clock_; Factory factory_;
    std::unique_ptr<ServiceConnection> service_; bool stopped_ = false;
    bool local_work_ = false;
};
}
