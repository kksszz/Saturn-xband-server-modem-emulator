#include "cable_runtime.h"
#include "link_cable_service.hpp"
#include <cstdlib>
#include <stdexcept>
#include <iostream>
using saturn::link::LinkCableService;
static void check(bool value){if(!value)throw std::runtime_error("Mednafen cable runtime contract");}
static int cancelled(){return 1;}
template<class F>static void until(F test){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);while(!test()){check(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
int main(){
    _putenv_s("XBAND_MEDNAFEN_CABLE","0");check(xband_cable_start(1)&&!xband_cable_enabled());
    _putenv_s("XBAND_MEDNAFEN_CABLE","1");check(!xband_cable_start(1)&&!xband_cable_enabled());
    _putenv_s("XBAND_MEDNAFEN_CABLE_PORT","bad");check(!xband_cable_start(0));
    _putenv_s("XBAND_MEDNAFEN_CABLE_PORT","59380");_putenv_s("XBAND_MEDNAFEN_CABLE_SYNC","0");
    check(xband_cable_start(0)&&xband_cable_enabled());
    LinkCableService peer;peer.SetDualSCI(true);peer.AutoConnectLocal(59380);
    until([&]{return xband_cable_connected()&&peer.GetState()==LinkCableService::State::Connected;});
    xband_cable_send(1,0x00,10);xband_cable_send(0,0xff,10);
    std::optional<saturn::link::uint8> v;
    until([&]{v=peer.ReceiveSCI(true,0);return v.has_value();});check(*v==0);
    until([&]{v=peer.ReceiveSCI(false,0);return v.has_value();});check(*v==255);
    peer.TransmitSCI(false,0x33,0);int b=-1;until([&]{b=xband_cable_receive(0,0);return b>=0;});check(b==0x33&&xband_cable_receive(1,0)==-1);
    xband_cable_reset();check(!xband_cable_connected()&&xband_cable_receive(0,0)==-1);
    xband_cable_stop();peer.Disconnect();
    _putenv_s("XBAND_MEDNAFEN_CABLE_SYNC","1");_putenv_s("XBAND_MEDNAFEN_CABLE_PORT","59381");
    check(xband_cable_start(0));
    peer.SetSynchronized(true);peer.AutoConnectLocal(59381);
    until([&]{return xband_cable_connected()&&peer.GetState()==LinkCableService::State::Connected;});
    check(xband_cable_budget(0,0)==4096);
    xband_cable_set_cancel(cancelled);check(xband_cable_budget(4096,0)==~uint64_t(0));
    check(!xband_cable_connected());
    xband_cable_stop();peer.Disconnect();xband_cable_set_cancel(nullptr);
    std::cout<<"C ABI opt-in, modem exclusion, validation, dual SCI, reset and quit cancellation passed\n";
}
