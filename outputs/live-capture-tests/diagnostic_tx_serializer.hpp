#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <stdexcept>
#include <limits>
// Bounded diagnostic TX holding FIFO + shift register. Fixed experimental clock.
class DiagnosticTxSerializer {
    std::deque<uint8_t> holding;
    bool shifting=false;
    uint8_t shift=0;
    uint64_t due=0;
public:
    bool holdingEmpty() const {return holding.empty();}
    bool empty() const {return holding.empty()&&!shifting;}
    bool push(uint8_t byte) {if(holding.size()==16)return false;holding.push_back(byte);return true;}
    void clear() {holding.clear();shifting=false;due=0;}
    void clearHolding() {holding.clear();}
    void tick(uint64_t now,uint64_t period,const std::function<bool(uint8_t)> &send) {
        if(!period)throw std::invalid_argument("zero TX period");
        if(shifting&&now>=due) {
            if(!send(shift))return; // Consumer false means not accepted; retain exactly once.
            shifting=false;
        }
        if(!shifting&&!holding.empty()) {
            if(now>std::numeric_limits<uint64_t>::max()-period)throw std::overflow_error("TX deadline");
            shift=holding.front();holding.pop_front();shifting=true;due=now+period;
        }
    }
};
