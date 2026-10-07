#pragma once
#include <xband/windows_modem_service.hpp>
#include <chrono>
#include <cstdlib>
#include "pb3_wait.hpp"
// Migration-only driver: invoked between RunFrame calls, never from UART/MMIO.
// Uses the existing v2 TCP transport. No local PPP/application response fallback.
class PB3RemoteService {
public:
    struct Timing {uint64_t ioNs=0,batchNs=0,selectNs=0,selectCalls=0,timeouts=0;};
private:
    Timing timing_;
    static uint64_t nanos(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    using Service=xband::windows::ModemServiceClient;
    std::unique_ptr<Service> client;
    unsigned side;
    uint64_t sent=0,received=0;
    inline static std::vector<Service*> connections;
    static void maintainAll(){
        for(auto *c:connections)if(!c->step(now()))throw std::runtime_error("PB3 remote transport lost");
    }
    static uint64_t now(){return GetTickCount64();}
    template<class Done> void wait(Done done){
        const auto end=now()+5000;
        while(now()<end){
            auto started=nanos();
            maintainAll();
            timing_.ioNs+=nanos()-started;
            started=nanos();const bool complete=done();timing_.batchNs+=nanos()-started;
            if(complete)return;
            fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
            bool canWait=true;
            for(auto *c:connections)canWait=c->appendWaitSockets(readable,writable)&&canWait;
            if(canWait){timeval timeout{0,1000};
                started=nanos();const auto ready=select(0,&readable,&writable,nullptr,&timeout);
                timing_.selectNs+=nanos()-started;++timing_.selectCalls;if(!ready)++timing_.timeouts;
                if(ready==SOCKET_ERROR)throw std::runtime_error("PB3 readiness wait failed");
            }
        }
        throw std::runtime_error("PB3 remote server timeout");
    }
public:
    Timing timing()const{return timing_;}
    explicit PB3RemoteService(unsigned index,unsigned channel=0):side(index){
        if(side>1||channel>2)throw std::invalid_argument("PB3 endpoint selection");
        const std::array<const char*,3> names{"pb3-","peer-","call-"};
        client=std::make_unique<Service>(xband::windows::ClientConfig{
            "127.0.0.1",static_cast<uint16_t>(58240+channel*2+side),false,names[channel]+std::to_string(side),std::string(64,'a'),60},now());
        connections.push_back(client.get());
        try{wait([&]{return client->state()==Service::State::idle;});}
        catch(...){std::erase(connections,client.get());throw;}
    }
    ~PB3RemoteService(){std::erase(connections,client.get());}
    void maintain(){maintainAll();}
    void close(){
        maintain();
        if(client->state()==Service::State::connected){
            if(!client->close(now()))throw std::runtime_error("PB3 remote close rejected");
            wait([&]{return client->state()==Service::State::idle;});
        }
    }
    void open(){
        maintain();
        if(client->state()==Service::State::idle){
            if(!client->open(side?"3336666665":"3336666666",now()))
                throw std::runtime_error("PB3 remote open rejected");
            wait([&]{return client->state()==Service::State::connected;});
        }
    }
    template<class Sink> void transfer(std::span<const uint8_t> bytes,unsigned frame,Sink sink){
        open();
        // Only this diagnostic driver waits. The shared service owner advances
        // bounded work per step and is reusable from an application's event loop.
        if(!client->transfer(bytes,frame))throw std::runtime_error("PB3 remote batch rejected");
        wait([&]{return client->readyToRun();});
        sent+=bytes.size();
        client->drain([&](uint8_t b){if(!sink(b))return false;++received;return true;},65536);
        if(client->state()==Service::State::stopped)throw std::runtime_error("PB3 remote response delivery failed");
        if(client->pending())throw std::runtime_error("PB3 remote response overflow");
    }
};
