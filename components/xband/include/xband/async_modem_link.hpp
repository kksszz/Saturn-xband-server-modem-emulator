#pragma once
#include <array>
#include <deque>
#include <span>
#include <vector>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace xband {
// Independent modem clocks. No peer CPU grants, frame lockstep or silent drops.
class AsyncModemRelay {
    std::array<std::deque<uint8_t>,2> queues_;
    std::array<uint64_t,2> sequence_{},elapsed_{};
    uint64_t generation_;
public:
    static constexpr size_t capacity=4096,chunk=256;
    explicit AsyncModemRelay(uint64_t generation):generation_(generation){if(!generation)throw std::invalid_argument("zero async generation");}
    std::vector<uint8_t> exchange(unsigned side,uint64_t generation,uint64_t sequence,uint64_t elapsed,
                                  std::span<const uint8_t> tx,size_t credit){
        if(side>1||generation!=generation_||sequence_[side]==UINT64_MAX||sequence!=sequence_[side]+1||elapsed<elapsed_[side]||
           tx.size()>chunk||credit>chunk||queues_[1-side].size()+tx.size()>capacity)
            throw std::runtime_error("Invalid async modem exchange or full relay");
        // Validate first, then commit exactly once. No relationship between clocks.
        sequence_[side]=sequence;elapsed_[side]=elapsed;
        auto &out=queues_[1-side];out.insert(out.end(),tx.begin(),tx.end());
        auto &in=queues_[side];std::vector<uint8_t> rx;rx.reserve(std::min(credit,in.size()));
        while(credit&&!in.empty()){--credit;rx.push_back(in.front());in.pop_front();}return rx;
    }
    size_t pending(unsigned side)const{return queues_.at(side).size();}
};
// Network arrivals are staged outside the UART FIFO, then released on the
// receiver's own guest clock. period is an explicit compatibility parameter.
class AsyncModemReceive {
    std::deque<uint8_t> bytes_;
    uint64_t due_=0;
public:
    static constexpr size_t capacity=4096;
    void reset(){bytes_.clear();due_=0;}
    size_t pending()const{return bytes_.size();}
    size_t credit()const{return std::min<size_t>(256,capacity-bytes_.size());}
    // Called after tick(). If the UART FIFO is full, its next read / owner poll
    // will retry; an overdue byte must not make the CPU busy-loop.
    uint64_t cyclesUntilEvent(uint64_t now)const{
        return !bytes_.empty()&&due_>now?due_-now:std::numeric_limits<uint64_t>::max();
    }
    void accept(std::span<const uint8_t> bytes,uint64_t now,uint64_t period){
        if(!period||bytes_.size()+bytes.size()>capacity||now>UINT64_MAX-period)throw std::runtime_error("Invalid async receive buffer/timing");
        if(bytes_.empty()&&!bytes.empty())due_=now+period;
        bytes_.insert(bytes_.end(),bytes.begin(),bytes.end());
    }
    template<class Sink> void tick(uint64_t now,uint64_t period,Sink sink){
        if(!period||now>UINT64_MAX-period)throw std::runtime_error("Invalid async receive period");
        if(bytes_.empty()||now<due_||!sink(bytes_.front()))return;
        bytes_.pop_front();due_=now+period; // no burst after suspension
    }
};
}
