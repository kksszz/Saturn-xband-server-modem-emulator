#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>

namespace xband {
// Emulator-owned, owner-thread only. UART callbacks enqueue; the execution
// loop performs transport work. No sockets, emulator headers or service logic.
class ModemServiceBuffer {
public:
    enum class State {idle,dialing,connected};
    static constexpr size_t capacity=4096;
    bool dial(std::string_view number){
        if(state_!=State::idle||number.empty()||number.size()>32)return false;
        target_=number;state_=State::dialing;return true;
    }
    void confirmConnected(){
        if(state_!=State::dialing)throw std::logic_error("service open without pending dial");
        state_=State::connected;
    }
    State state()const noexcept{return state_;}
    bool carrier()const noexcept{return state_==State::connected;}
    std::string_view target()const noexcept{return target_;}
    bool transmit(uint8_t byte){
        if(!carrier()||used_==capacity)return false;
        tx_[used_++]=byte;return true;
    }
    std::span<const uint8_t> outgoing()const noexcept{return {tx_.data(),used_};}
    // Only after transport has accepted the entire span. Never silently retry.
    void transmitted()noexcept{tx_.fill(0);used_=0;}
    bool receive(uint8_t byte){
        if(!carrier()||rx_.size()==capacity)return false;
        rx_.push_back(byte);return true;
    }
    bool takeReceived(uint8_t &byte){
        if(rx_.empty())return false;byte=rx_.front();rx_.pop_front();return true;
    }
    void reset()noexcept{state_=State::idle;target_.clear();transmitted();rx_.clear();}
private:
    State state_=State::idle;
    std::string target_;
    std::array<uint8_t,capacity> tx_{};
    size_t used_=0;
    std::deque<uint8_t> rx_;
};
}
