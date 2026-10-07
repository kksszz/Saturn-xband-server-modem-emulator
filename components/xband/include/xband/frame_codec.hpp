#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace xband {
// Opaque uint32 big-endian length framing only. UTF-8/JSON/schema/authentication
// validation MUST occur before dispatch. Owner-thread, no IO or allocation.
class FrameDecoder {
public:
    static constexpr size_t max_payload=65536;
    enum class Status {need_more,ready,invalid_length};
    struct Result {size_t consumed; Status status;};
    Result feed(std::span<const uint8_t> input) noexcept {
        if(status_!=Status::need_more)return {0,status_};
        size_t consumed=0;
        for(auto byte:input) {
            ++consumed;
            if(header_bytes_<4) {
                expected_=(expected_<<8)|byte;
                if(++header_bytes_==4 && (expected_==0 || expected_>max_payload)) {
                    status_=Status::invalid_length;return {consumed,status_};
                }
            } else {
                payload_[used_++]=byte;
                if(used_==expected_){status_=Status::ready;return {consumed,status_};}
            }
        }
        return {consumed,status_};
    }
    // Read-only borrowed view, invalidated by consume/reset/mutation/destruction.
    std::span<const uint8_t> peek() const noexcept {
        return status_==Status::ready ? std::span<const uint8_t>(payload_.data(),used_)
                                     : std::span<const uint8_t>{};
    }
    bool consume() noexcept {
        if(status_!=Status::ready)return false;
        reset();return true;
    }
    // After a fault reset only for a fresh transport generation. Never append
    // bytes from a new session to fragments from the old connection.
    void reset() noexcept {expected_=0;header_bytes_=0;used_=0;status_=Status::need_more;}
private:
    std::array<uint8_t,max_payload> payload_{};
    uint32_t expected_=0;
    unsigned header_bytes_=0;
    size_t used_=0;
    Status status_=Status::need_more;
};
enum class EncodeStatus {ok,invalid_length,insufficient_space};
struct EncodeResult {EncodeStatus status;size_t written;};
// Input/output must not overlap. Failure leaves output unchanged. Caller owns
// encoded bytes and must resume partial socket writes at its own write cursor.
inline EncodeResult encodeFrame(std::span<const uint8_t> payload,std::span<uint8_t> output) noexcept {
    if(payload.empty()||payload.size()>FrameDecoder::max_payload)return {EncodeStatus::invalid_length,0};
    if(output.size()<payload.size()+4)return {EncodeStatus::insufficient_space,0};
    auto length=static_cast<uint32_t>(payload.size());
    output[0]=static_cast<uint8_t>(length>>24);output[1]=static_cast<uint8_t>(length>>16);
    output[2]=static_cast<uint8_t>(length>>8);output[3]=static_cast<uint8_t>(length);
    for(size_t i=0;i<payload.size();++i)output[i+4]=payload[i];
    return {EncodeStatus::ok,payload.size()+4};
}
} // namespace xband
