// SPDX-License-Identifier: GPL-3.0-only
#include "cable_runtime.h"
#include "link_cable_service.hpp"
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
namespace {
using saturn::link::LinkCableService;
std::unique_ptr<LinkCableService> cable;
std::uint64_t revision=1;
int (*cancelled)()=nullptr;
int previousState=-1;
std::chrono::steady_clock::time_point lastReport;
std::uint64_t lastSent=0,lastReceived=0,lastDelivered=0;
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
        cable=std::make_unique<LinkCableService>();
        cable->SetDualSCI(true);cable->SetSynchronized(sync);
        cable->AutoConnectLocal(uint16_t(port));
        previousState=-1;
        lastReport={};lastSent=lastReceived=lastDelivered=0;
        std::fprintf(stderr,"[Link cable] Local YLS1 dual SCI; port=%u sync=%u; experimental\n",port,unsigned(sync));
        return 1;
    }catch(const std::exception& e){std::fprintf(stderr,"[Link cable] %s\n",e.what());xband_cable_stop();return 0;}
}
extern "C" void xband_cable_stop(){cable.reset();++revision;previousState=-1;}
extern "C" int xband_cable_enabled(){return cable!=nullptr;}
extern "C" int xband_cable_connected(){return cable&&cable->GetState()==LinkCableService::State::Connected;}
extern "C" void xband_cable_set_cancel(int (*fn)(void)){cancelled=fn;}
extern "C" void xband_cable_poll(){
    if(!cable)return;
    const int state=int(cable->GetState());
    if(state!=previousState){
        previousState=state;
        const char* names[]={"Offline","Listening","Connecting","Connected to peer","Error"};
        std::fprintf(stderr,"[Link cable] %s %s\n",names[state],cable->GetError().c_str());
    }
    const auto now=std::chrono::steady_clock::now();
    if(now-lastReport>=std::chrono::seconds(2)){
        lastReport=now;
        const auto d=cable->GetDiagnostics();
        if(d.sent!=lastSent||d.received!=lastReceived||d.delivered!=lastDelivered){
            lastSent=d.sent;lastReceived=d.received;lastDelivered=d.delivered;
            std::fprintf(stderr,"[Link cable] bytes TX=%llu RX=%llu delivered=%llu pending=%zu\n",
                static_cast<unsigned long long>(d.sent),static_cast<unsigned long long>(d.received),
                static_cast<unsigned long long>(d.delivered),d.receiveQueue);
        }
    }
}
extern "C" void xband_cable_reset(){
    if(!cable)return;
    ++revision;
    // Do not carry a socket or old queued bytes across guest reset/state load.
    // Restart the same local port is explicit: restart both instances.
    cable->Disconnect();
}
extern "C" uint64_t xband_cable_budget(uint64_t cycle,unsigned clock){
    if(!cable)return ~uint64_t(0);
    const auto budget=cable->AdvanceSynchronized(cycle,revision,clock,[]{
        return cancelled&&cancelled();
    });
    if(budget==0){cable->AbortSynchronization("Link cancelled for game shutdown");return ~uint64_t(0);}
    return budget;
}
extern "C" void xband_cable_send(int master,uint8_t value,uint64_t cycle){if(cable)cable->TransmitSCI(master!=0,value,cycle);}
extern "C" int xband_cable_receive(int master,uint64_t cycle){
    if(!cable)return -1;
    const auto value=cable->ReceiveSCI(master!=0,cycle);
    return value?int(*value):-1;
}
