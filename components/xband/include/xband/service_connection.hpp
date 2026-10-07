#pragma once
#include <xband/service_control.hpp>
#include <xband/service_transfer.hpp>
#include <memory>
#include <optional>
#include <stdexcept>

namespace xband::protocol {
// Post-authentication local-service host, one owner thread, no reentrant calls.
// Lifecycle outlives this object. Host supplies verified identity and fresh tokens;
// this class does NOT authenticate, reserve endpoints, send hello_ok, or own sockets.
class ServiceConnection {
public:
    using Outcome = ServiceTransfer::Outcome;
    ServiceConnection(ConnectionLifecycle &connection, uint64_t generation,
                      std::string endpoint, std::string session,
                      std::unique_ptr<ServiceEndpoint> service, FrameClock clock)
        : connection_(connection), generation_(generation), inbox_(connection, generation),
          outbox_(queue_, connection, generation, std::move(endpoint), std::move(session)),
          service_(std::move(service)), clock_(clock) {
        if (!service_) throw std::invalid_argument("Missing service endpoint");
        control_.emplace(inbox_, connection_, generation_, outbox_, *service_);
    }
    ServiceConnection(const ServiceConnection &) = delete;
    ServiceConnection &operator=(const ServiceConnection &) = delete;
    ~ServiceConnection() { disconnect(); }

    ServerInbox::FeedResult feed(std::span<const uint8_t> bytes, uint64_t now) noexcept {
        if (!live(now)) return {0, ServerInbox::Status::failed};
        try {
            auto result = inbox_.feed(bytes, now);
            if (result.status == ServerInbox::Status::failed) disconnect();
            return result;
        } catch (...) { disconnect(); return {0, ServerInbox::Status::failed}; }
    }
    // Call even with no received bytes to enforce host-time deadlines. new_call
    // is needed only for open; a blocked retry must retain the same token.
    Outcome poll(uint64_t now, std::string_view new_call = {}, size_t budget = 4096) noexcept {
        if (!live(now)) return Outcome::failed;
        try {
            if (ping_id_ && now - ping_start_ >= ConnectionLifecycle::request_ms) return fail();
            if (inbox_.peek()) {
                const auto route = inbox_.route();
                if (route == ServerInbox::Route::pong) {
                    if (!ping_id_ || (*inbox_.peek())["reply_to"] != std::to_string(ping_id_)) return fail();
                    ping_id_ = 0; inbox_.consume(); return Outcome::handled;
                }
                if (route == ServerInbox::Route::snapshot) {
                    const auto &body = (*inbox_.peek())["body"];
                    if (control_->call().empty()) {
                        if (!body["call"].is_null() || !body["subscriber"].is_null() ||
                            (body["state"] != "idle" && body["state"] != "opening" && body["state"] != "error")) return fail();
                    } else if (!body["call"].is_string() || body["call"].get_ref<const std::string &>() != control_->call() ||
                        !body["subscriber"].is_string() || body["subscriber"].get_ref<const std::string &>() != control_->subscriber() ||
                        (body["state"] != "service" && body["state"] != "closing" && body["state"] != "error")) return fail();
                    snapshot_ = body; inbox_.consume(); return Outcome::handled;
                }
                if (route == ServerInbox::Route::error) return fail(); // peer error: no automatic replay
                if (route == ServerInbox::Route::service || route == ServerInbox::Route::ping) {
                    const auto previous_call = std::string(control_->call());
                    const auto result = control_->process(new_call);
                    if (result == ServiceControl::Outcome::failed) return fail();
                    if (result == ServiceControl::Outcome::blocked) return Outcome::blocked;
                    if (previous_call != control_->call()) snapshot_ = nullptr;
                    // Atomic with control's reset: no transfer pump can run in
                    // between. Closing cancels staged input/unfinished advance.
                    if (control_->call().empty()) transfer_.reset();
                    else if (!transfer_) transfer_ = std::make_unique<ServiceTransfer>(
                        inbox_, outbox_, connection_, generation_, *service_, clock_, std::string(control_->call()));
                    return Outcome::handled;
                }
                if (!transfer_ || (route != ServerInbox::Route::data &&
                    route != ServerInbox::Route::acknowledgement && route != ServerInbox::Route::clock)) return fail();
                const auto result = transfer_->process();
                if (result == Outcome::failed) return fail();
                return result;
            }
            if (!ping_id_ && connection_.pingDue()) {
                const auto id = outbox_.nextId();
                const auto result = outbox_.control("ping", nlohmann::json::object());
                if (result == Admission::queue_full) return Outcome::blocked;
                if (result != Admission::queued || !connection_.pingQueued(generation_, now)) return fail();
                ping_id_ = id; ping_start_ = now;
            }
            if (!transfer_) return Outcome::idle;
            const auto result = transfer_->drive(budget);
            return result == Outcome::failed ? fail() : result;
        } catch (...) { return fail(); }
    }
    std::string_view call() const noexcept { return current() && control_ ? control_->call() : std::string_view{}; }
    // Untrusted peer-reported status, for display only. Never a card ledger or
    // input to the emulated service. Borrowed view invalidated by next mutation.
    const nlohmann::json *snapshot() const noexcept { return current() && !snapshot_.is_null() ? &snapshot_ : nullptr; }
    std::span<const uint8_t> output() const noexcept { return current() ? queue_.peek() : std::span<const uint8_t>{}; }
    // Consume only bytes actually sent by the socket, on this same owner thread.
    bool sent(size_t bytes) noexcept {
        if (!current() || !queue_.consume(bytes)) { disconnect(); return false; }
        return true;
    }
    void disconnect() noexcept {
        snapshot_ = nullptr; ping_id_ = ping_start_ = 0;
        outbox_.discard(); transfer_.reset(); control_.reset(); service_.reset();
        stopped_ = true;
        if (connection_.generation() == generation_) connection_.close();
    }
private:
    bool current() const noexcept {
        return !stopped_ && connection_.generation() == generation_ &&
            connection_.state() == ConnectionLifecycle::State::active;
    }
    bool live(uint64_t now) noexcept {
        if (current() && connection_.poll(generation_, now)) return true;
        disconnect(); return false;
    }
    Outcome fail() noexcept { disconnect(); return Outcome::failed; }
    ConnectionLifecycle &connection_; uint64_t generation_;
    ServerInbox inbox_; FrameQueue queue_; SessionOutbox outbox_;
    std::unique_ptr<ServiceEndpoint> service_; FrameClock clock_;
    std::optional<ServiceControl> control_;
    std::unique_ptr<ServiceTransfer> transfer_;
    nlohmann::json snapshot_;
    uint64_t ping_id_ = 0, ping_start_ = 0;
    bool stopped_ = false;
};
}
