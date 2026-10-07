#pragma once
#include <cstdint>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>

namespace xband {
// Timing subset extracted from the successful catalog replay. The caller owns
// the clock and experimental periods; these are not physical hardware claims.
class PeerUartTiming {
    std::deque<uint8_t> holding;
    bool shifting=false;
    uint8_t shift=0;
    uint64_t due=0,lastActivity=0;
public:
    void reset(){holding.clear();shifting=false;due=lastActivity=0;}
    void clearHolding(){holding.clear();}
    bool push(uint8_t byte){if(holding.size()==16)return false;holding.push_back(byte);return true;}
    uint8_t txStatus()const{return uint8_t((holding.empty()?0x20:0)|(!shifting&&holding.empty()?0x40:0));}
    void activity(uint64_t now){lastActivity=now;}
    // Query after tick(). An already-due blocked TX is retried by the owner
    // when its output queue drains, not by forcing one-cycle CPU slices.
    uint64_t cyclesUntilEvent(uint64_t now,uint8_t ier,uint8_t fcr,size_t bytes,uint64_t timeout)const{
        auto result=std::numeric_limits<uint64_t>::max();
        if(shifting&&due>now)result=due-now;
        constexpr unsigned levels[]{1,4,8,14};
        if((ier&1)&&(fcr&1)&&bytes&&bytes<levels[(fcr>>6)&3]&&timeout&&now>=lastActivity&&now-lastActivity<timeout){
            const auto remaining=timeout-(now-lastActivity);
            if(remaining<result)result=remaining;
        }
        return result;
    }
    uint8_t interrupt(uint8_t ier,uint8_t fcr,size_t bytes,uint64_t now,uint64_t timeout)const{
        if(!(ier&1)||!bytes)return 1;
        constexpr unsigned levels[]{1,4,8,14};
        if(!(fcr&1)||bytes>=levels[(fcr>>6)&3])return 4;
        return timeout&&now>=lastActivity&&now-lastActivity>=timeout?12:1;
    }
    template<class Send> void tick(uint64_t now,uint64_t period,Send send){
        if(!period)throw std::invalid_argument("zero UART TX period");
        if(shifting&&now>=due){if(!send(shift))return;shifting=false;}
        if(!shifting&&!holding.empty()){
            if(now>std::numeric_limits<uint64_t>::max()-period)throw std::overflow_error("UART TX deadline");
            shift=holding.front();holding.pop_front();shifting=true;due=now+period;
        }
    }
};
}
