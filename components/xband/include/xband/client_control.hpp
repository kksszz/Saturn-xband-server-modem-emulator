#pragma once
#include <xband/session_outbox.hpp>
#include <memory>

namespace xband::protocol {
// Emulator-side control channel foundation. Owner thread; no sockets, sleeps,
// Ymir dependency or card writes. One instance per TCP connection; never reuse
// after EOF/reset. Guest ticks are explicit protocol values, not wall time.
// The emulator clock adapter and production sockets remain external.
class ClientControl {
public:
    enum class State { hello, idle, opening, service, closing, stopped };
    ClientControl(std::string name, std::string key, uint64_t clock_hz, uint64_t now)
        : generation_(lifecycle_.begin(now)), name_(std::move(name)) {
        const auto hello = nlohmann::json{{"v", 2}, {"type", "hello"}, {"endpoint", name_}, {"session", ""},
            {"id", "1"}, {"reply_to", nullptr}, {"body", {{"client", "xband-emulator-adapter"}, {"clock_hz", clock_hz}, {"auth_key", key}}}}.dump();
        if (!parseWire(hello) || !lifecycle_.hello(generation_, name_, now) ||
            queue_.enqueue(std::span(reinterpret_cast<const uint8_t *>(hello.data()), hello.size())) != FrameQueue::Result::ok) disconnect();
    }
    ClientControl(const ClientControl &) = delete;
    ClientControl &operator=(const ClientControl &) = delete;
    State state() const noexcept { return state_; }
    std::string_view call() const noexcept { return call_; }
    std::span<const uint8_t> output() const noexcept { return queue_.peek(); }
    bool sent(size_t count) noexcept {
        if (state_ == State::stopped || count > queue_.peek().size()) { disconnect(); return false; }
        queue_.consume(count); return true;
    }
    void disconnect() noexcept {
        state_ = State::stopped; lifecycle_.close(); queue_.reset(); inbox_.reset();
        outbox_.reset(); call_.clear(); subscriber_.clear(); session_.clear(); request_ = 0;
        resetTransfer(); validated_ = false;
    }
    MessageInbox::FeedResult feed(std::span<const uint8_t> bytes, uint64_t now) noexcept {
        if (!poll(now)) return {0, MessageInbox::Status::failed};
        try {
            auto result = inbox_.feed(bytes);
            if (result.status == MessageInbox::Status::failed || !poll(now)) {
                disconnect(); result.status = MessageInbox::Status::failed;
            }
            return result;
        } catch (...) { disconnect(); return {0, MessageInbox::Status::failed}; }
    }
    bool poll(uint64_t now) noexcept {
        try {
            if (state_ == State::stopped || !lifecycle_.poll(generation_, now)) { disconnect(); return false; }
            if (!flushAck()) return fail();
            const auto *message = inbox_.peek();
            if (!message) return true;
            const auto &m = *message;
            const auto type = m["type"].get<std::string>();
            if (state_ == State::hello) {
                if (!queue_.peek().empty() || type != "hello_ok" || m["endpoint"] != name_ || m["id"] != "1" || m["reply_to"] != "1")
                    return fail();
                session_ = m["body"]["new_session"].get<std::string>();
                if (!lifecycle_.authenticationCompleted(generation_, true, session_, now)) return fail();
                outbox_ = std::make_unique<SessionOutbox>(queue_, lifecycle_, generation_, name_, session_);
                state_ = State::idle;
            } else {
                // Validate once: a full pong output queue retains this input.
                if (!validated_) {
                    if (type != "ping" && type != "open_ok" && type != "close_ok" && type != "error" &&
                        type != "data" && type != "data_ack" && type != "advance_ok") return fail();
                    if (!lifecycle_.received(generation_, 2, m["endpoint"].get<std::string>(),
                            m["session"].get<std::string>(), m["id"].get<std::string>(), now)) return fail();
                    validated_ = true;
                }
                if (type == "ping") {
                    const auto admitted = outbox_->control("pong", nlohmann::json::object(), m["id"]);
                    if (admitted == Admission::queue_full) return true;
                    if (admitted != Admission::queued) return fail();
                } else if (type == "open_ok" && state_ == State::opening && m["reply_to"] == std::to_string(request_)) {
                    if (!lifecycle_.requestCompleted(generation_, request_, now)) return fail();
                    call_ = m["body"]["call"].get<std::string>(); resetTransfer(); state_ = State::service; request_ = 0;
                } else if (type == "close_ok" && state_ == State::closing && m["reply_to"] == std::to_string(request_) && m["body"]["call"] == call_) {
                    if (!lifecycle_.requestCompleted(generation_, request_, now)) return fail();
                    call_.clear(); subscriber_.clear(); resetTransfer(); state_ = State::idle; request_ = 0;
                } else if (type == "data_ack" && (state_ == State::service || state_ == State::closing) && m["body"]["call"] == call_) {
                    const auto &b = m["body"]; uint64_t next = 0, limit = 0;
                    if (!decimal(b["next_offset"].get<std::string>(), next) || !decimal(b["limit"].get<std::string>(), limit) ||
                        sender_.acknowledge(next, limit) != Result::ok) return fail();
                } else if (type == "data" && state_ == State::service && advancing_ && m["body"]["call"] == call_) {
                    const auto &b = m["body"]; uint64_t offset = 0; size_t count = 0;
                    std::array<uint8_t, ReceiveWindow::max_chunk> decoded{};
                    if (!decimal(b["offset"].get<std::string>(), offset) || !decodeBase64(b["payload_b64"].get<std::string>(), decoded, count) ||
                        receiver_.accept(offset, std::span(decoded).first(count)) != Result::ok) return fail();
                    ack_dirty_ = true;
                    if (!flushAck()) return fail();
                } else if (type == "advance_ok" && state_ == State::service && advancing_ &&
                           m["body"]["call"] == call_ && m["reply_to"] == std::to_string(request_) &&
                           m["body"]["tick"] == std::to_string(pending_tick_)) {
                    if (sender_.acknowledged() != sender_.nextOffset() || !lifecycle_.requestCompleted(generation_, request_, now)) return fail();
                    completed_tick_ = pending_tick_; advancing_ = false; request_ = 0;
                } else return fail();
            }
            inbox_.consume(); validated_ = false; return true;
        } catch (...) { return fail(); }
    }
    Admission open(std::string subscriber, uint64_t now) noexcept {
        try {
            if (!poll(now) || state_ != State::idle) return Admission::inactive;
            const auto id = outbox_->nextId();
            const auto result = outbox_->control("open", {{"target", "local-service"}, {"subscriber", subscriber}});
            if (result == Admission::queued) {
                if (!lifecycle_.requestQueued(generation_, id, now)) { disconnect(); return Admission::inactive; }
                request_ = id; subscriber_ = std::move(subscriber); state_ = State::opening;
            }
            return result;
        } catch (...) { disconnect(); return Admission::inactive; }
    }
    Admission close(uint64_t now) noexcept {
        try {
            if (!poll(now) || state_ != State::service || advancing_) return Admission::inactive;
            const auto id = outbox_->nextId();
            const auto result = outbox_->control("close", {{"call", call_}, {"reason", "hangup"}});
            if (result == Admission::queued) {
                if (!lifecycle_.requestQueued(generation_, id, now)) { disconnect(); return Admission::inactive; }
                request_ = id; state_ = State::closing;
            }
            return result;
        } catch (...) { disconnect(); return Admission::inactive; }
    }
    Admission transmit(std::span<const uint8_t> bytes, uint64_t now) noexcept {
        try {
            if (!poll(now) || state_ != State::service || advancing_) return Admission::inactive;
            if (bytes.empty() || bytes.size() > ReceiveWindow::max_chunk) return Admission::invalid;
            std::string encoded;
            if (!encodeBase64(bytes, encoded)) return Admission::invalid;
            return outbox_->data(sender_, call_, {{"call", call_}, {"offset", std::to_string(sender_.nextOffset())},
                {"bytes", bytes.size()}, {"payload_b64", std::move(encoded)}});
        } catch (...) { disconnect(); return Admission::inactive; }
    }
    Admission advance(uint64_t guest_tick, uint64_t now) noexcept {
        try {
            if (!poll(now) || state_ != State::service || advancing_) return Admission::inactive;
            if (guest_tick < completed_tick_) return Admission::invalid;
            const auto id = outbox_->nextId();
            const auto result = outbox_->control("advance", {{"call", call_}, {"tick", std::to_string(guest_tick)}});
            if (result == Admission::queued) {
                if (!lifecycle_.requestQueued(generation_, id, now)) { disconnect(); return Admission::inactive; }
                request_ = id; pending_tick_ = guest_tick; advancing_ = true;
            }
            return result;
        } catch (...) { disconnect(); return Admission::inactive; }
    }
    bool advancing() const noexcept { return advancing_; }
    uint64_t completedTick() const noexcept { return completed_tick_; }
    // Output can arrive before advance_ok: consume it to release credit, but do
    // not start the next batch until advancing() becomes false.
    std::span<const uint8_t> received() const noexcept { return receiver_.peek(); }
    bool consumeReceived(size_t count, uint64_t now) noexcept {
        if (!poll(now) || state_ != State::service) return false;
        if (receiver_.consume(count) != Result::ok) return fail();
        ack_dirty_ = true;
        try { if (!flushAck()) return fail(); } catch (...) { return fail(); }
        return true;
    }
    // Card body is already a copy from the emulator; never reads or mutates a
    // card store. Transport fields come from this connection, not the caller.
    Admission snapshot(nlohmann::json card, uint64_t sent_bytes, uint64_t received_bytes, uint64_t now) noexcept {
        try {
            if (!poll(now) || (state_ != State::idle && state_ != State::service)) return Admission::inactive;
            const bool service = state_ == State::service;
            if (!service && (sent_bytes || received_bytes)) return Admission::invalid;
            return outbox_->control("snapshot", {{"call", service ? nlohmann::json(call_) : nlohmann::json(nullptr)},
                {"subscriber", service ? nlohmann::json(subscriber_) : nlohmann::json(nullptr)}, {"state", service ? "service" : "idle"},
                {"sent_bytes", std::to_string(sent_bytes)}, {"received_bytes", std::to_string(received_bytes)}, {"card", std::move(card)}});
        } catch (...) { disconnect(); return Admission::inactive; }
    }
private:
    void resetTransfer() noexcept {
        sender_.reset(); receiver_.reset(); advancing_ = ack_dirty_ = false; pending_tick_ = completed_tick_ = 0;
    }
    bool flushAck() {
        if (!ack_dirty_) return true;
        if (!outbox_ || call_.empty()) return false;
        const auto result = outbox_->control("data_ack", {{"call", call_}, {"next_offset", std::to_string(receiver_.nextOffset())},
            {"limit", std::to_string(receiver_.limit())}});
        if (result == Admission::queue_full) return true; // retain latest credit, retry on poll
        if (result != Admission::queued) return false;
        ack_dirty_ = false; return true;
    }
    bool fail() noexcept { disconnect(); return false; }
    ConnectionLifecycle lifecycle_; uint64_t generation_, request_ = 0;
    FrameQueue queue_; MessageInbox inbox_; std::unique_ptr<SessionOutbox> outbox_;
    std::string name_, session_, call_, subscriber_;
    State state_ = State::hello; bool validated_ = false;
    SendWindow sender_; ReceiveWindow receiver_;
    uint64_t pending_tick_ = 0, completed_tick_ = 0;
    bool advancing_ = false, ack_dirty_ = false;
};
}
