#pragma once
#include "local_peer_line.hpp"
#include "pb3_remote_service.hpp"
// Cached PeerLine view. UART calls only enqueue intents; flush() performs IPC
// between guest execution slices. Game bytes remain opaque to this adapter.
class PB3RemoteCall final:public LocalPeerLine {
    PB3RemoteService channel;
    unsigned side,tick=0;
    State cached=State::Idle;
    uint64_t token=0;
    bool hangupPending=false;
    nlohmann::json pending={{"op","join"}};
    std::array<bool,2> joined{},finished{};
    std::vector<uint8_t> tx;
    std::deque<uint8_t> rx;
    uint64_t grantEnd=0;
public:
    PB3RemoteService::Timing timing()const{return channel.timing();}
    explicit PB3RemoteCall(unsigned s):channel(s,2),side(s){}
    void flush(){
        if(++tick==0)throw std::runtime_error("call control tick overflow");
        if(hangupPending){pending={{"op","hangup"},{"generation",token}};hangupPending=false;}
        const auto text=pending.dump();std::string response;
        channel.transfer(std::span(reinterpret_cast<const uint8_t*>(text.data()),text.size()),tick,
            [&](uint8_t c){if(response.size()>=4096)return false;response.push_back(char(c));return true;});
        const auto r=nlohmann::json::parse(response);
        const auto newToken=r.at("generation").get<uint64_t>();
        if(token&&token!=newToken)throw std::runtime_error("call generation changed");
        token=newToken;const auto value=r.at("state").get<unsigned>();
        if(value>2)throw std::runtime_error("invalid remote call state");
        cached=static_cast<State>(value);joined=r.at("joined").get<std::array<bool,2>>();finished=r.at("finished").get<std::array<bool,2>>();
        grantEnd=r.at("grant_end").get<uint64_t>();
        const auto bytes=r.at("bytes").get<std::vector<unsigned>>();
        if(rx.size()+bytes.size()>256)throw std::runtime_error("remote receive queue overflow");
        for(auto b:bytes){if(b>255)throw std::runtime_error("invalid remote byte");rx.push_back(static_cast<uint8_t>(b));}
        pending={{"op","poll"}};
    }
    uint64_t step(uint64_t elapsed){
        pending={{"op","step"},{"generation",token},{"elapsed",elapsed},{"bytes",tx}};
        flush();tx.clear();return grantEnd;
    }
    bool bothJoined()const{return joined[0]&&joined[1];}
    bool bothFinished()const{return finished[0]&&finished[1];}
    void finish(){pending={{"op","finish"},{"generation",token}};flush();}
    State state()const override{return cached;}
    uint64_t session()const override{return token;}
    bool ringing(unsigned s)const override{return s==1&&cached==State::Ringing;}
    bool dial(unsigned s)override{
        if(s!=side||s!=0||cached!=State::Idle||pending.at("op")!="poll"||!bothJoined())return false;
        pending={{"op","dial"},{"number","3336666665"}};return true;
    }
    bool answer(unsigned s,uint64_t generation)override{
        if(s!=side||s!=1||generation!=token||!ringing(s)||pending.at("op")!="poll")return false;
        pending={{"op","answer"},{"generation",token}};return true;
    }
    // Also invoked by DiagnosticModemRoute's destructor: no network, allocation
    // or exception here. During execution the next flush reports the intent;
    // during teardown socket closure is the authoritative disconnect.
    bool hangup(unsigned s,uint64_t generation)override{
        if(s!=side||generation!=token)return false;
        cached=State::Idle;hangupPending=true;return true;
    }
    Send send(unsigned s,uint64_t generation,uint8_t byte)override{
        if(s!=side||generation!=token||cached!=State::Connected)return Send::NoCarrier;
        if(tx.size()==256)return Send::Backpressure;
        tx.push_back(byte);++sent[side];return Send::Delivered;
    }
    bool receive(unsigned s,uint64_t generation,uint8_t &byte)override{
        if(s!=side||generation!=token||cached!=State::Connected||rx.empty())return false;
        byte=rx.front();rx.pop_front();++received[side];return true;
    }
    Snapshot snapshot()const override{return {cached,token,cached==State::Idle?-1:0,{0,0},{0,0},{0,0}};}
};
