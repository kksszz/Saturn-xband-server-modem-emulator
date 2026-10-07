#pragma once
#include <cstdint>

namespace xband {
// Emulator-independent board boundary. Physical address; width is 1, 2 or 4.
// Values use the guest's big-endian bus order. Debug accesses must not consume
// receive bytes, issue commands or advance the device. Owner-thread only.
class DeviceBus {
public:
    virtual ~DeviceBus()=default;
    virtual uint32_t read(uint32_t address,unsigned width,bool peek)=0;
    virtual void write(uint32_t address,uint32_t value,unsigned width,bool poke)=0;
};
}
