#pragma once
#include <xband/connection_lifecycle.hpp>
#include <xband/message_pipeline.hpp>

namespace xband::protocol {
// Server-side, post-hello message gate. Lifecycle must outlive this owner-thread
// object. A fresh inbox is required for each connection generation. This gate
// shares the lifecycle's sequence guard; never call received() a second time.
class ServerInbox {
public:
    enum class Route { none, service, data, acknowledgement, clock, snapshot, ping, pong, error };
    using Status = MessageInbox::Status;
    using FeedResult = MessageInbox::FeedResult;
    ServerInbox(ConnectionLifecycle &connection, uint64_t generation) noexcept
        : connection_(connection), generation_(generation) {}
    FeedResult feed(std::span<const uint8_t> bytes, uint64_t now) {
        if (failed_) return {0, Status::failed};
        if (!current() || !connection_.poll(generation_, now)) { fail(); return {0, Status::failed}; }
        if (route_ != Route::none) return {0, Status::ready};
        FeedResult result{};
        try { result = inbox_.feed(bytes); }
        catch (...) { fail(); throw; }
        if (result.status == Status::failed) { fail(); return result; }
        if (result.status != Status::ready) return result;
        const auto &message = *inbox_.peek();
        const auto &type = message["type"].get_ref<const std::string &>();
        Route route = Route::none;
        if (type == "open" || type == "close") route = Route::service;
        else if (type == "data") route = Route::data;
        else if (type == "data_ack") route = Route::acknowledgement;
        else if (type == "advance") route = Route::clock;
        else if (type == "snapshot") route = Route::snapshot;
        else if (type == "ping") route = Route::ping;
        else if (type == "pong") route = Route::pong;
        else if (type == "error") route = Route::error;
        // hello/hello_ok and server-only replies are never valid at this gate.
        if (route == Route::none || !connection_.received(generation_, 2,
            message["endpoint"].get_ref<const std::string &>(),
            message["session"].get_ref<const std::string &>(),
            message["id"].get_ref<const std::string &>(), now)) {
            fail(); return {result.consumed, Status::failed};
        }
        route_ = route; return result;
    }
    Route route() const noexcept { return current() && !failed_ ? route_ : Route::none; }
    const nlohmann::json *peek() const noexcept {
        return route() != Route::none ? inbox_.peek() : nullptr;
    }
    // Call only after the selected handler accepts the message. A blocked
    // handler leaves it pending; observing it must not perform game side effects.
    bool consume() noexcept {
        if (!peek()) return false;
        inbox_.consume(); route_ = Route::none; return true;
    }
private:
    bool current() const noexcept {
        return connection_.generation() == generation_ && connection_.state() == ConnectionLifecycle::State::active;
    }
    void fail() noexcept {
        failed_ = true; route_ = Route::none; inbox_.reset();
        if (connection_.generation() == generation_) connection_.close();
    }
    ConnectionLifecycle &connection_;
    uint64_t generation_;
    MessageInbox inbox_;
    Route route_ = Route::none;
    bool failed_ = false;
};
}
