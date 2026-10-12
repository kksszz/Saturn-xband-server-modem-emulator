// SPDX-License-Identifier: GPL-3.0-only
#include "cable_runtime.h"
#include "link_cable_service.hpp"
#include "local_sci_cable.hpp"
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
namespace {
using saturn::link::LinkCableService;
std::unique_ptr<LinkCableService> cable;
std::unique_ptr<saturn::link::LocalSciCable> localCable;
std::uint64_t revision=1;
int (*cancelled)()=nullptr;
int previousState=-1;
std::chrono::steady_clock::time_point lastReport;
std::uint64_t lastSent=0,lastReceived=0,lastDelivered=0;
std::uint64_t cachedBoundary=0,cachedRevision=0,cachedSession=0,lastBudgetCycle=0;
unsigned cachedClock=0;
bool budgetCached=false;
LinkCableService::State currentState(){
    return localCable?localCable->GetState():cable?cable->GetState():LinkCableService::State::Disconnected;
}
void invalidateBudget(){budgetCached=false;lastBudgetCycle=0;}
}
extern "C" int xband_cable_start(int modem_cart) {
    xband_cable_stop();
    const char* enable=std::getenv("XBAND_MEDNAFEN_CABLE");
    if(!enable||std::string(enable)=="0")return 1;
    try {
        if(std::string(enable)!="1")throw std::runtime_error("XBAND_MEDNAFEN_CABLE must be 0 or 1");
        if(modem_cart)throw std::runtime_error("Link cable and XBAND modem are mutually exclusive");
        unsigned port=32458;
        if(const char* text=std::getenv("XBAND_MEDNAFEN_CABLE_PORT")) {
            char* end=nullptr;const long n=std::strtol(text,&end,10);
            if(!*text||!end||*end||n<1024||n>65535)throw std::runtime_error("Invalid link port");
            port=unsigned(n);
        }
        bool sync=true;
        if(const char* text=std::getenv("XBAND_MEDNAFEN_CABLE_SYNC")) {
            if(std::string(text)!="0"&&std::string(text)!="1")throw std::runtime_error("Invalid link synchronization setting");
            sync=std::string(text)=="1";
        }
        const char* backendValue=std::getenv("XBAND_MEDNAFEN_CABLE_BACKEND");
        const std::string backend=backendValue?backendValue:"tcp";
        unsigned quantum=4096;
        if(const char* text=std::getenv("XBAND_MEDNAFEN_CABLE_QUANTUM")){
            const std::string value(text);
            if(value!="64"&&value!="128"&&value!="256"&&value!="512"&&
               value!="1024"&&value!="2048"&&value!="4096")
                throw std::runtime_error("Invalid link quantum");
            quantum=unsigned(std::strtoul(text,nullptr,10));
        }
        if(backend=="shared"){
            if(!sync)throw std::runtime_error("Shared SCI requires synchronization ON");
            localCable=std::make_unique<saturn::link::LocalSciCable>(port,quantum);
        }else if(backend=="tcp"){
            if(quantum!=4096)throw std::runtime_error("YLS1 TCP requires quantum 4096; use shared for adjustable latency");
            cable=std::make_unique<LinkCableService>();
            cable->SetDualSCI(true);cable->SetSynchronized(sync);
            cable->AutoConnectLocal(uint16_t(port));
        }else throw std::runtime_error("Link backend must be tcp or shared");
        previousState=-1;
        lastReport={};lastSent=lastReceived=lastDelivered=0;
        std::fprintf(stderr,"[Link cable] backend=%s dual SCI; channel/port=%u sync=%u quantum=%u; experimental\n",
                     backend.c_str(),port,unsigned(sync),quantum);
        return 1;
    }catch(const std::exception& e){std::fprintf(stderr,"[Link cable] %s\n",e.what());xband_cable_stop();return 0;}
}
extern "C" void xband_cable_stop(){localCable.reset();cable.reset();++revision;previousState=-1;invalidateBudget();}
extern "C" int xband_cable_enabled(){return cable!=nullptr||localCable!=nullptr;}
extern "C" int xband_cable_connected(){return currentState()==LinkCableService::State::Connected;}
extern "C" void xband_cable_set_cancel(int (*fn)(void)){cancelled=fn;}
extern "C" void xband_cable_poll(){
    if(!xband_cable_enabled())return;
    const int state=int(currentState());
    if(state!=previousState){
        previousState=state;
        const char* names[]={"Offline","Listening","Connecting","Connected to peer","Error"};
        const auto error=localCable?localCable->GetError():cable->GetError();
        std::fprintf(stderr,"[Link cable] %s %s\n",names[state],error.c_str());
    }
    const auto now=std::chrono::steady_clock::now();
    if(now-lastReport>=std::chrono::seconds(2)){
        lastReport=now;
        const auto d=localCable?localCable->GetDiagnostics():cable->GetDiagnostics();
        if(d.sent!=lastSent||d.received!=lastReceived||d.delivered!=lastDelivered){
            lastSent=d.sent;lastReceived=d.received;lastDelivered=d.delivered;
            std::fprintf(stderr,"[Link cable] bytes TX=%llu RX=%llu delivered=%llu pending=%zu\n",
                static_cast<unsigned long long>(d.sent),static_cast<unsigned long long>(d.received),
                static_cast<unsigned long long>(d.delivered),d.receiveQueue);
        }
    }
}
extern "C" void xband_cable_reset(){
    if(!xband_cable_enabled())return;
    ++revision;
    invalidateBudget();
    // Do not carry a socket or old queued bytes across guest reset/state load.
    // Restart the same local port is explicit: restart both instances.
    if(localCable)localCable->Disconnect();
    if(cable)cable->Disconnect();
}
extern "C" uint64_t xband_cable_budget(uint64_t cycle,unsigned clock){
    if(!xband_cable_enabled())return ~uint64_t(0);
    if(cancelled&&cancelled()){
        if(localCable)localCable->Disconnect();
        if(cable)cable->AbortSynchronization("Link cancelled for game shutdown");
        invalidateBudget();return ~uint64_t(0);
    }
    const auto state=currentState();
    const auto session=localCable?localCable->SessionEpoch():cable->SessionEpoch();
    if(budgetCached&&state==LinkCableService::State::Connected&&revision==cachedRevision&&
       clock==cachedClock&&session==cachedSession&&cycle>=lastBudgetCycle&&cycle<cachedBoundary){
        lastBudgetCycle=cycle;return cachedBoundary-cycle;
    }
    budgetCached=false;
    const auto budget=localCable?localCable->Advance(cycle,revision,clock,cancelled):
        cable->AdvanceSynchronized(cycle,revision,clock,[]{return cancelled&&cancelled();});
    if(budget==0){
        if(localCable)localCable->Disconnect();
        if(cable)cable->AbortSynchronization("Link cancelled for game shutdown");
        return ~uint64_t(0);
    }
    if(budget!=~uint64_t(0)&&cycle<=~uint64_t(0)-budget){
        cachedBoundary=cycle+budget;cachedClock=clock;cachedRevision=revision;
        cachedSession=localCable?localCable->SessionEpoch():cable->SessionEpoch();
        lastBudgetCycle=cycle;budgetCached=true;
    }
    return budget;
}
extern "C" void xband_cable_send(int master,uint8_t value,uint64_t cycle){
    if(localCable)localCable->Send(master!=0,value,cycle);
    else if(cable)cable->TransmitSCI(master!=0,value,cycle);
}
extern "C" int xband_cable_receive(int master,uint64_t cycle){
    if(localCable)return localCable->Receive(master!=0,cycle);
    if(!cable)return -1;
    const auto value=cable->ReceiveSCI(master!=0,cycle);
    return value?int(*value):-1;
}
