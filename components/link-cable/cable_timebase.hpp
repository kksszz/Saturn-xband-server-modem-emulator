// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace saturn { namespace link {
// C++11 host clock: normalize transport time to master-clock / 65 ticks.
// SCI register/baud timing continues to use the unscaled SH-2 cycle clock.
class CableTimebase {
    std::uint64_t masterBase_=0;
    static std::uint64_t span(std::uint64_t cycles,unsigned divider){
        if((divider!=61&&divider!=65)||cycles>std::numeric_limits<std::uint64_t>::max()/divider)
            throw std::runtime_error("Invalid cable CPU clock span");
        return cycles*divider;
    }
public:
    std::uint64_t sample(std::uint64_t frameCycles,unsigned divider)const{
        const auto amount=span(frameCycles,divider);
        if(amount>std::numeric_limits<std::uint64_t>::max()-masterBase_)
            throw std::runtime_error("Cable clock overflow");
        return (masterBase_+amount)/65;
    }
    void endFrame(std::uint64_t frameCycles,unsigned divider){
        const auto amount=span(frameCycles,divider);
        if(amount>std::numeric_limits<std::uint64_t>::max()-masterBase_)
            throw std::runtime_error("Cable clock overflow");
        masterBase_+=amount; // Keep the fractional tick across frame rebases.
    }
};
} }
