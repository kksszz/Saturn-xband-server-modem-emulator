// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <limits>

namespace xband { namespace mednafen {
// Owner-thread, bounded sub-frame network pumping. Guest UART timing is separate.
class PeerPollSchedule {
    uint64_t nextCycle_=0,nextWall_=0;
public:
    static constexpr uint64_t cycleInterval=4096,wallInterval=1;
    void reset(){nextCycle_=nextWall_=0;}
    bool cycleDue(uint64_t cycle)const{return cycle>=nextCycle_;}
    bool admit(uint64_t cycle,uint64_t wall){
        if(!cycleDue(cycle))return false;
        const auto maximum=std::numeric_limits<uint64_t>::max();
        nextCycle_=cycle>maximum-cycleInterval?maximum:cycle+cycleInterval;
        if(wall<nextWall_)return false;
        nextWall_=wall==maximum?maximum:wall+wallInterval;
        return true;
    }
    static uint64_t controlDelay(bool active){return active?1:16;}
};
} }
