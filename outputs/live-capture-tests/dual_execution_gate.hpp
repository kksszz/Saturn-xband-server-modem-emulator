#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

// Diagnostic single-threaded cooperative gate. Uses Saturn scheduler time,
// NOT StepMasterSH2's returned instruction cycles. No transport or guest data.
class DualExecutionGate {
public:
    static constexpr uint64_t quantum=4096;
    // Experimental safety ceiling, not a claimed hardware instruction maximum.
    static constexpr uint64_t maxInstructionOverrun=64;
    struct Clock { bool initialized=false; uint64_t origin=0,revision=0,elapsed=0,grantEnd=0,lastGrant=0,maxOverrun=0; };
    std::array<Clock,2> clocks{};
    uint64_t budget(unsigned side,uint64_t cycle,uint64_t revision) {
        if(side>1)throw std::out_of_range("gate side");
        auto &c=clocks[side];
        if(!c.initialized) {c={true,cycle,revision,0};return 0;}
        if(revision!=c.revision||cycle<c.origin||cycle-c.origin<c.elapsed)
            throw std::runtime_error("dual gate timeline changed");
        c.elapsed=cycle-c.origin;
        if(c.elapsed>c.grantEnd) {
            const auto excess=c.elapsed-c.grantEnd;
            if(!c.lastGrant||excess>maxInstructionOverrun)throw std::runtime_error("dual gate invalid grant overrun: "+std::to_string(excess));
            c.maxOverrun=std::max(c.maxOverrun,excess);
        }
        if(!clocks[1-side].initialized)return 0;
        const auto peer=clocks[1-side].elapsed;
        if(c.elapsed>peer&&c.elapsed-peer>quantum+maxInstructionOverrun)
            throw std::runtime_error("dual gate exceeded lead bound: side="+std::to_string(side)+" elapsed="+std::to_string(c.elapsed)+" peer="+std::to_string(peer)+" lead="+std::to_string(c.elapsed-peer));
        if(peer>UINT64_MAX-quantum)throw std::runtime_error("dual gate clock overflow");
        const auto limit=std::min(stopAt,peer+quantum);
        // Charge all executed cycles, including an instruction overrun, against
        // the next grant. Never clamp the observed clock or grant extra credit.
        c.lastGrant=c.elapsed>=limit?0:std::min(quantum,limit-c.elapsed);
        if(c.elapsed>UINT64_MAX-c.lastGrant)throw std::runtime_error("dual gate grant overflow");
        c.grantEnd=c.elapsed+c.lastGrant;
        return c.lastGrant;
    }
    uint64_t stopAt=0; // Set only while neither core is running.
};
