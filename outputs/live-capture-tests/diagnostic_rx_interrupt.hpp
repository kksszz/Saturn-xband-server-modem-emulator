#pragma once
#include <cstdint>
#include <cstddef>
// RX-only diagnostic condition, not a complete UART or physical baud model.
struct DiagnosticRxInterrupt {
    uint64_t lastActivity=0;
    void activity(uint64_t now) {lastActivity=now;}
    unsigned code(uint8_t ier,uint8_t fcr,size_t bytes,uint64_t now,uint64_t timeout) const {
        if(!(ier&1)||!bytes)return 1;
        if(!(fcr&1))return 4;
        constexpr unsigned levels[]{1,4,8,14};
        if(bytes>=levels[(fcr>>6)&3])return 4;
        return timeout&&now>=lastActivity&&now-lastActivity>=timeout?12:1;
    }
};
