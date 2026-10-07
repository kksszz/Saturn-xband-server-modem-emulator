#pragma once
#include "local_peer_line.hpp"
#include "pb3_remote_service.hpp"
// Migration adapter. Control/gating remains in the dual-core runner; payloads
// cross the external server. Never performs socket IO from send/receive/MMIO.
class PB3RemotePeer final:public LocalPeerLine {
    std::array<std::unique_ptr<PB3RemoteService>,2> links;
    std::array<std::vector<uint8_t>,2> tx;
    std::array<std::deque<uint8_t>,2> rx;
    unsigned transaction=0;
public:
    void flush(){
        if(state()!=State::Connected)return;
        if(!links[0]){for(unsigned i=0;i<2;++i)links[i]=std::make_unique<PB3RemoteService>(i,true);
            // Open/reset both remote endpoints before admitting any payload.
            for(unsigned i=0;i<2;++i)links[i]->transfer({},transaction,[](uint8_t){throw std::runtime_error("stale relay data at open");return false;});
        }
        if(tx[0].empty()&&tx[1].empty())return;
        if(++transaction==0)throw std::runtime_error("relay transaction overflow");
        // Send both sides, then fetch the opposite direction after all sends.
        for(unsigned i=0;i<2;++i){
            links[i]->transfer(tx[i],transaction,[&,i](uint8_t b){if(rx[i].size()>=capacity)return false;rx[i].push_back(b);return true;});
            tx[i].clear();
        }
        links[0]->transfer({},transaction,[&](uint8_t b){if(rx[0].size()>=capacity)return false;rx[0].push_back(b);return true;});
    }
    Send send(unsigned side,uint64_t token,uint8_t byte)override{
        if(side>1)throw std::out_of_range("relay side");
        if(state()!=State::Connected||token!=session())return Send::NoCarrier;
        if(tx[side].size()>=capacity)return Send::Backpressure;
        tx[side].push_back(byte);++sent[side];
        if(observePayload)observePayload(side,token,byte);
        return Send::Delivered;
    }
    bool receive(unsigned side,uint64_t token,uint8_t &byte)override{
        if(side>1)throw std::out_of_range("relay side");
        if(state()!=State::Connected||token!=session()||rx[side].empty())return false;
        byte=rx[side].front();rx[side].pop_front();++received[side];return true;
    }
    Snapshot snapshot()const override{
        auto s=LocalPeerLine::snapshot();s.pendingReceive={rx[0].size(),rx[1].size()};s.sent=sent;s.received=received;return s;
    }
    bool hangup(unsigned side,uint64_t token)override{
        if(!LocalPeerLine::hangup(side,token))return false;
        for(auto &q:tx)q.clear();for(auto &q:rx)q.clear();
        // Socket teardown is deferred to destruction/outside guest execution.
        return true;
    }
};
