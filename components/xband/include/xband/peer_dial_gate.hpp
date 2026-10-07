#pragma once
#include <cstdint>
#include <stdexcept>

namespace xband {
// Owner-thread gate only: wait for a fresh control snapshot, never invent a role.
class PeerDialGate {
public:
    enum class Result { none, wait, ready, reject };
    void reset(){active_=false;}
    bool active()const{return active_;}
    void arm(uint64_t generation,uint32_t frame){
        if(active_||!generation)throw std::logic_error("Invalid pending peer dial");
        active_=true;generation_=generation;start_=frame;
    }
    Result observe(uint64_t generation,uint32_t frame,unsigned side,unsigned caller,
                   bool joined,bool idle,bool closed){
        if(!active_)return Result::none;
        if(side>1||caller>2||generation!=generation_||closed||
           uint32_t(frame-start_)>=300||(caller<2&&caller!=side)){
            reset();return Result::reject;
        }
        if(caller==side&&joined&&idle){reset();return Result::ready;}
        return Result::wait;
    }
private:
    bool active_=false;
    uint64_t generation_=0;
    uint32_t start_=0;
};
}
