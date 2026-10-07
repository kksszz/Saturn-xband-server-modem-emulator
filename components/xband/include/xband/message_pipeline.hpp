#pragma once
#include <xband/frame_queue.hpp>
#include <xband/send_window.hpp>
#include <xband/wire_json.hpp>

namespace xband::protocol {
// One validated message held until explicitly consumed. This layer does not
// authenticate or dispatch; the owner must check session/call/sequence first.
class MessageInbox {
public:
    enum class Status { need_more, ready, failed };
    struct FeedResult { size_t consumed; Status status; };
    FeedResult feed(std::span<const uint8_t> input) {
        if (status_ != Status::need_more) return {0, status_};
        const auto result = decoder_.feed(input);
        if (result.status == FrameDecoder::Status::invalid_length) status_ = Status::failed;
        else if (result.status == FrameDecoder::Status::ready) {
            // Fail closed if allocation/parsing throws: caller must close the
            // connection and must not retry these bytes on this instance.
            status_ = Status::failed;
            const auto bytes = decoder_.peek();
            message_ = parseWire(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            if (message_) status_ = Status::ready;
        }
        return {result.consumed, status_};
    }
    const nlohmann::json *peek() const noexcept {
        return status_ == Status::ready ? &message_.value : nullptr;
    }
    bool consume() noexcept {
        if (status_ != Status::ready) return false;
        message_.value = nullptr; decoder_.consume(); status_ = Status::need_more; return true;
    }
    void reset() noexcept {
        decoder_.reset(); message_.value = nullptr; message_.error = WireError::syntax; status_ = Status::need_more;
    }
private:
    FrameDecoder decoder_;
    WireMessage message_;
    Status status_ = Status::need_more;
};

enum class Admission { queued, queue_full, credit_full, invalid, inactive };
// A serialized data frame's queue admission and send-window accounting form one
// owner-thread transaction. No callbacks between enqueue and accounting commit.
// Caller verifies outbound endpoint/session/ID policy and advances its message ID
// ONLY on queued. Never call again to account for partial socket writes.
inline Admission enqueueData(FrameQueue &queue, SendWindow &window,
                             std::string_view current_call, std::string_view json) {
    if (window.failed()) return Admission::inactive;
    const auto message = parseWire(json);
    if (!message || message.value["type"] != "data" || message.value["body"]["call"].get_ref<const std::string &>() != current_call)
        return Admission::invalid;
    const auto &body = message.value["body"];
    uint64_t offset = 0;
    if (!decimal(body["offset"].get_ref<const std::string &>(), offset) || offset != window.nextOffset())
        return Admission::invalid;
    auto next = window; // Stage accounting; rejected admission leaves original untouched.
    const auto credit = next.commit(body["bytes"].get<size_t>());
    if (credit == Result::capacity) return Admission::credit_full;
    if (credit != Result::ok) return Admission::invalid;
    const auto result = queue.enqueue(std::span(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
    if (result == FrameQueue::Result::full) return Admission::queue_full;
    if (result != FrameQueue::Result::ok) return Admission::invalid;
    window = next;
    return Admission::queued;
}
}
