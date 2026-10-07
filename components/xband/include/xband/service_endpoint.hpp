#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace xband {
// Transitional source API extracted from the diagnostic runner, not a stable ABI.
// Owner-thread only; frame is legacy emulated frames, NOT milliseconds or ticks.
class ServiceEndpoint {
public:
    virtual ~ServiceEndpoint() = default;
    // false means unaccepted: caller retains the byte. No blocking network IO.
    virtual bool transmit(uint8_t byte,unsigned frame) = 0;
    virtual void tick(unsigned frame) = 0;
    virtual bool peek(uint8_t &byte) const = 0;
    virtual void consume() = 0;
    virtual size_t pending() const = 0;
    virtual void reset() = 0;
};
// Legacy exceptions retained for parity. Future network/C ABI boundaries must
// catch failures and end the affected session instead of exposing exceptions.
template<class Sink>
bool pumpService(ServiceEndpoint &service,std::span<const uint8_t> input,
                 size_t &cursor,unsigned frame,Sink sink) {
    if(cursor>input.size())throw std::invalid_argument("service cursor outside captured input");
    while(cursor<input.size()) {
        if(!service.transmit(input[cursor],frame))return false;
        ++cursor;
    }
    service.tick(frame);
    uint8_t byte;
    while(service.peek(byte)) {
        if(!sink(byte))return false;
        service.consume();
    }
    return true;
}
} // namespace xband
