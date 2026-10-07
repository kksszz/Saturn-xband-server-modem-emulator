#pragma once
#include "peer_line.hpp"
#include <array>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <limits>
// Single-threaded, same-process diagnostic wire for two future modem adapters.
// No phone numbers, sockets, PPP, automatic answering or fabricated peer data.
class LocalPeerLine : public PeerLine {
public:
    // Owner-thread only. Does not advance pacing or consume queued bytes.
    Snapshot snapshot() const override {
        return {status,generation,status==State::Idle?-1:int(caller),
                {queues[0].size(),queues[1].size()},sent,received};
    }
    static constexpr size_t capacity=256;
    std::array<uint64_t,2> sent{},received{}; // Lifetime diagnostic payload counters, excluding modem responses.
    std::function<void(unsigned,uint64_t,uint8_t)> observePayload;
    // Optional diagnostic serialization. Configure only with an empty wire.
    // Fixed scheduler units; does not claim a physical modem clock or UART FIFO.
    void setBytePacing(std::function<uint64_t()> clock,uint64_t cycles) {
        if(!clock||!cycles||!queues[0].empty()||!queues[1].empty())throw std::invalid_argument("invalid pacing setup");
        pacingClock=std::move(clock);byteCycles=cycles;lastDue={};
    }
    State state() const override {return status;}
    uint64_t session() const override {return generation;}
    bool ringing(unsigned side) const override {valid(side);return status==State::Ringing && side!=caller;}
    bool dial(unsigned side) override {
        valid(side);
        if(status!=State::Idle)return false;
        clear();caller=side;++generation;status=State::Ringing;return true;
    }
    bool answer(unsigned side,uint64_t token) override {
        valid(side);
        if(token!=generation || !ringing(side))return false;
        status=State::Connected;return true;
    }
    bool hangup(unsigned side,uint64_t token) override {
        valid(side);if(token!=generation)return false;
        clear();status=State::Idle;return true;
    }
    Send send(unsigned side,uint64_t token,uint8_t byte) override {
        valid(side);
        if(status!=State::Connected || token!=generation)return Send::NoCarrier;
        auto &q=queues[1-side];
        if(q.size()==capacity)return Send::Backpressure;
        uint64_t due=0;
        if(pacingClock) {
            const auto start=std::max(pacingClock(),lastDue[side]);
            if(start>std::numeric_limits<uint64_t>::max()-byteCycles)throw std::overflow_error("wire clock overflow");
            due=start+byteCycles;lastDue[side]=due;
        }
        q.push_back({byte,due});++sent[side];
        if(observePayload)observePayload(side,generation,byte);
        return Send::Delivered;
    }
    bool receive(unsigned side,uint64_t token,uint8_t &byte) override {
        valid(side);
        if(status!=State::Connected || token!=generation || queues[side].empty())return false;
        if(pacingClock&&pacingClock()<queues[side].front().due)return false;
        byte=queues[side].front().byte;queues[side].pop_front();++received[side];return true;
    }
private:
    State status=State::Idle;
    unsigned caller=0;
    uint64_t generation=0;
    struct Pending {uint8_t byte;uint64_t due;};
    std::array<std::deque<Pending>,2> queues;
    std::function<uint64_t()> pacingClock;
    uint64_t byteCycles=0;
    std::array<uint64_t,2> lastDue{};
    static void valid(unsigned side) {if(side>1)throw std::out_of_range("peer side");}
    void clear() {for(auto &q:queues)q.clear();lastDue={};}
};
