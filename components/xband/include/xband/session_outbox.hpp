#pragma once
#include <xband/connection_lifecycle.hpp>
#include <xband/message_pipeline.hpp>

namespace xband::protocol {
// One shared sequencer for ALL post-hello output on one owner-thread connection.
// Queue and lifecycle references must outlive it; no direct queue admissions by
// other producers. Socket code only peeks/consumes. Exceptions precede admission.
class SessionOutbox {
public:
    SessionOutbox(FrameQueue &queue, ConnectionLifecycle &connection, uint64_t generation,
                  std::string endpoint_name, std::string session, uint64_t first_id = 2)
        : queue_(queue), connection_(connection), generation_(generation),
          endpoint_(std::move(endpoint_name)), session_(std::move(session)), next_(first_id) {
        valid_ = endpoint(endpoint_) && token(session_) && first_id != 0;
    }
    Admission control(std::string_view type, nlohmann::json body, nlohmann::json reply_to = nullptr) {
        if (!active()) return Admission::inactive;
        if (type == "data" || type == "hello" || type == "hello_ok") return Admission::invalid;
        const auto text = envelope(type, std::move(body), std::move(reply_to)).dump();
        if (!parseWire(text)) return Admission::invalid;
        const auto result = queue_.enqueue(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        if (result == FrameQueue::Result::full) return Admission::queue_full;
        if (result != FrameQueue::Result::ok) return Admission::invalid;
        committed(); return Admission::queued;
    }
    Admission data(SendWindow &window, std::string_view current_call, nlohmann::json body) {
        if (!active()) return Admission::inactive;
        const auto text = envelope("data", std::move(body), nullptr).dump();
        const auto result = enqueueData(queue_, window, current_call, text);
        if (result == Admission::queued) committed();
        return result;
    }
    uint64_t nextId() const noexcept { return next_; }
    bool exhausted() const noexcept { return exhausted_; }
    void discard() noexcept { queue_.reset(); valid_ = false; }
private:
    bool active() const noexcept {
        return valid_ && !exhausted_ && connection_.generation() == generation_ &&
            connection_.state() == ConnectionLifecycle::State::active;
    }
    nlohmann::json envelope(std::string_view type, nlohmann::json body, nlohmann::json reply) const {
        return {{"v", 2}, {"type", type}, {"endpoint", endpoint_}, {"session", session_},
            {"id", std::to_string(next_)}, {"reply_to", std::move(reply)}, {"body", std::move(body)}};
    }
    void committed() noexcept {
        if (next_ == std::numeric_limits<uint64_t>::max()) exhausted_ = true;
        else ++next_;
    }
    FrameQueue &queue_;
    ConnectionLifecycle &connection_;
    uint64_t generation_;
    std::string endpoint_, session_;
    uint64_t next_;
    bool valid_ = false, exhausted_ = false;
};
}
