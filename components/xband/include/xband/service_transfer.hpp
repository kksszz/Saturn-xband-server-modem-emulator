#pragma once
#include <xband/server_inbox.hpp>
#include <xband/session_outbox.hpp>
#include <xband/timed_service.hpp>
#include <algorithm>

namespace xband::protocol {
// One active service call. Host creates after open and discards atomically with
// close/reset, before any subsequent pump. No reentrant service callbacks.
// References belong to the same connection generation and outlive this object.
class ServiceTransfer {
public:
    enum class Outcome { idle, handled, progress, blocked, other_handler, failed };
    ServiceTransfer(ServerInbox &inbox, SessionOutbox &outbox, ConnectionLifecycle &connection,
                    uint64_t generation, ServiceEndpoint &service, FrameClock clock, std::string call)
        : inbox_(inbox), outbox_(outbox), connection_(connection), generation_(generation),
          pump_(service, clock), call_(std::move(call)) { failed_ = !token(call_); }
    // Process incoming ACKs even while drive() is blocked on output credit.
    Outcome process() noexcept {
        if (!active()) return Outcome::failed;
        const auto *m = inbox_.peek(); if (!m) return Outcome::idle;
        const auto route = inbox_.route();
        if (route != ServerInbox::Route::data && route != ServerInbox::Route::acknowledgement && route != ServerInbox::Route::clock)
            return Outcome::other_handler;
        try {
            const auto &b = (*m)["body"];
            if (b["call"].get_ref<const std::string &>() != call_) return fail();
            if (route == ServerInbox::Route::acknowledgement) {
                uint64_t next = 0, limit = 0;
                if (!number(b["next_offset"], next) || !number(b["limit"], limit) || sender_.acknowledge(next, limit) != Result::ok) return fail();
            } else if (route == ServerInbox::Route::data) {
                // Client must wait for advance_ok before starting the next batch.
                if (advancing_) return fail();
                if (!input_ack_pending_) {
                    uint64_t offset = 0; std::array<uint8_t, 4096> bytes{}; size_t size = 0;
                    if (!number(b["offset"], offset) || !decodeBase64(b["payload_b64"].get_ref<const std::string &>(), bytes, size) ||
                        pump_.accept(offset, std::span(bytes).first(size)) != Result::ok) return fail();
                    input_ack_pending_ = true;
                }
                const auto ack = sendAck(); if (ack != Outcome::handled) return ack;
                input_ack_pending_ = false;
            } else {
                uint64_t tick = 0;
                if (advancing_ || !number(b["tick"], tick)) return fail();
                request_id_ = (*m)["id"].get<std::string>();
                if (!pump_.beginAdvance(tick)) return fail();
                advancing_ = true; pump_done_ = false;
            }
            inbox_.consume(); return Outcome::handled;
        } catch (...) { return fail(); }
    }
    Outcome drive(size_t budget = 4096) noexcept {
        if (!active()) return Outcome::failed;
        if (!advancing_) return Outcome::idle;
        try {
            auto flushed = flush(); if (flushed != Outcome::handled) return flushed;
            auto step = TimedService::Step::done;
            if (!pump_done_) {
                step = pump_.resume([&](uint8_t byte) {
                    if (buffered_ == buffer_.size()) return false;
                    buffer_[buffered_++] = byte; return true;
                }, budget);
                if (step == TimedService::Step::failed) return fail();
                pump_done_ = step == TimedService::Step::done;
            }
            flushed = flush(); if (flushed != Outcome::handled) return flushed;
            const auto ack = sendAck(); if (ack != Outcome::handled) return ack;
            if (!pump_done_) return step == TimedService::Step::blocked ? Outcome::blocked : Outcome::progress;
            const auto queued = outbox_.control("advance_ok", {{"call", call_}, {"tick", std::to_string(pump_.completedTick())}}, request_id_);
            if (queued == Admission::queue_full) return Outcome::blocked;
            if (queued != Admission::queued) return fail();
            advancing_ = false; request_id_.clear(); return Outcome::handled;
        } catch (...) { return fail(); }
    }
    bool advancing() const noexcept { return advancing_; }
    size_t bufferedOutput() const noexcept { return buffered_; }
private:
    static bool number(const nlohmann::json &v, uint64_t &out) { return decimal(v.get_ref<const std::string &>(), out); }
    bool active() const noexcept {
        return !failed_ && connection_.generation() == generation_ && connection_.state() == ConnectionLifecycle::State::active;
    }
    Outcome sendAck() {
        if (ack_next_ == pump_.receivedOffset() && ack_limit_ == pump_.receiveLimit()) return Outcome::handled;
        const auto result = outbox_.control("data_ack", {{"call", call_}, {"next_offset", std::to_string(pump_.receivedOffset())},
            {"limit", std::to_string(pump_.receiveLimit())}});
        if (result == Admission::queue_full) return Outcome::blocked;
        if (result != Admission::queued) return fail();
        ack_next_ = pump_.receivedOffset(); ack_limit_ = pump_.receiveLimit(); return Outcome::handled;
    }
    Outcome flush() {
        if (buffered_ == 0) return Outcome::handled;
        const auto credit = sender_.limit() - sender_.nextOffset();
        const auto count = static_cast<size_t>(std::min<uint64_t>(buffered_, credit));
        if (count == 0) return Outcome::blocked;
        std::string encoded;
        if (!encodeBase64(std::span(buffer_).first(count), encoded)) return fail();
        const auto result = outbox_.data(sender_, call_, {{"call", call_}, {"offset", std::to_string(sender_.nextOffset())},
            {"bytes", count}, {"payload_b64", std::move(encoded)}});
        if (result == Admission::queue_full || result == Admission::credit_full) return Outcome::blocked;
        if (result != Admission::queued) return fail();
        std::move(buffer_.begin() + count, buffer_.begin() + buffered_, buffer_.begin()); buffered_ -= count;
        return buffered_ == 0 ? Outcome::handled : Outcome::blocked;
    }
    Outcome fail() noexcept {
        failed_ = true; buffered_ = 0; advancing_ = false; outbox_.discard();
        if (connection_.generation() == generation_) connection_.close();
        return Outcome::failed; // Host discards service/call state; no automatic replay.
    }
    ServerInbox &inbox_; SessionOutbox &outbox_; ConnectionLifecycle &connection_;
    uint64_t generation_;
    TimedService pump_; SendWindow sender_; std::string call_, request_id_;
    std::array<uint8_t, 4096> buffer_{}; size_t buffered_ = 0;
    uint64_t ack_next_ = 0, ack_limit_ = 65536;
    bool advancing_ = false, pump_done_ = false, input_ack_pending_ = false, failed_ = false;
};
}
