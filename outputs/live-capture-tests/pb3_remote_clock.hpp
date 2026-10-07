#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
// Cached execution grant only. Called by Ymir's scheduler; never performs IPC.
class PB3RemoteClock {
    bool initialized=false,enabled=false;
    uint64_t origin=0,revision=0,end=0;
public:
    uint64_t elapsed=0,maxOverrun=0;
    uint64_t budget(uint64_t cycle,uint64_t rev){
        if(!initialized){initialized=true;origin=cycle;revision=rev;return 0;}
        if(rev!=revision||cycle<origin||cycle-origin<elapsed)throw std::runtime_error("remote guest timeline changed");
        elapsed=cycle-origin;
        if(elapsed>end){
            const auto excess=elapsed-end;
            if(excess>64)throw std::runtime_error("remote guest grant overrun");
            maxOverrun=std::max(maxOverrun,excess);
        }
        return enabled&&elapsed<end?end-elapsed:0;
    }
    void grant(uint64_t limit){
        if(limit<elapsed||limit-elapsed>4096)throw std::runtime_error("invalid remote execution grant");
        end=limit;enabled=true;
    }
    // A zero-budget RunFrame samples the actual scheduler clock after a frame
    // boundary/debug break without executing guest instructions.
    void sampleOnly(){enabled=false;}
};
