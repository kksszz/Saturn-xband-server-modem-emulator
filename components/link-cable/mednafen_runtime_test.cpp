#include "cable_runtime.h"
#include "link_cable_service.hpp"
#include "local_sci_cable.hpp"
#include <future>
#include <cstdlib>
#include <stdexcept>
#include <iostream>
using saturn::link::LinkCableService;
static void check(bool value){if(!value)throw std::runtime_error("Mednafen cable runtime contract");}
static int cancelled(){return 1;}
template<class F>static void until(F test){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);while(!test()){check(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
int main(){
    _putenv_s("XBAND_MEDNAFEN_CABLE_BACKEND","");
    _putenv_s("XBAND_MEDNAFEN_CABLE_QUANTUM","");
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
    // The cached path must not conceal a clock change inside a quantum.
    _putenv_s("XBAND_MEDNAFEN_CABLE_PORT","59384");
    check(xband_cable_start(0));peer.AutoConnectLocal(59384);
    until([&]{return xband_cable_connected()&&peer.GetState()==LinkCableService::State::Connected;});
    check(xband_cable_budget(100,0)==4096);
    check(xband_cable_budget(164,0)==4032);
    // No owner budget calls during the disconnect/reconnect: a session epoch
    // must still invalidate the previous connection's cached boundary.
    peer.Disconnect();peer.AutoConnectLocal(59384);
    until([&]{return xband_cable_connected()&&peer.GetState()==LinkCableService::State::Connected;});
    check(xband_cable_budget(228,0)==4096);
    xband_cable_budget(228,1);check(!xband_cable_connected());
    xband_cable_stop();peer.Disconnect();
    _putenv_s("XBAND_MEDNAFEN_CABLE_BACKEND","unknown");check(!xband_cable_start(0));
    _putenv_s("XBAND_MEDNAFEN_CABLE_BACKEND","tcp");_putenv_s("XBAND_MEDNAFEN_CABLE_QUANTUM","128");
    check(!xband_cable_start(0)); // No incompatible small-quantum YLS1.
    _putenv_s("XBAND_MEDNAFEN_CABLE_BACKEND","shared");_putenv_s("XBAND_MEDNAFEN_CABLE_SYNC","0");
    check(!xband_cable_start(0));
    _putenv_s("XBAND_MEDNAFEN_CABLE_SYNC","1");
    _putenv_s("XBAND_MEDNAFEN_CABLE_QUANTUM","bad");check(!xband_cable_start(0));
    _putenv_s("XBAND_MEDNAFEN_CABLE_QUANTUM","128");
    _putenv_s("XBAND_MEDNAFEN_CABLE_PORT","59385");
    check(xband_cable_start(0));
    {
        saturn::link::LocalSciCable localPeer(59385,128);
        auto bound=std::async(std::launch::async,[]{return xband_cable_budget(100,0);});
        check(localPeer.Advance(900,1,0,nullptr)==128);check(bound.get()==128);
        check(xband_cable_connected()&&xband_cable_budget(104,0)==124);
        xband_cable_send(1,0x42,104);xband_cable_send(0,0x77,104);
        localPeer.Send(true,0xaa,904);localPeer.Send(false,0xbb,904);
        auto progress=std::async(std::launch::async,[]{return xband_cable_budget(228,0);});
        check(localPeer.Advance(1028,1,0,nullptr)==128);check(progress.get()==128);
        check(localPeer.Receive(true,1031)==-1&&localPeer.Receive(false,1032)==0x77);
        check(localPeer.Receive(true,1032)==0x42);
        check(xband_cable_receive(1,232)==0xaa&&xband_cable_receive(0,232)==0xbb);
        xband_cable_budget(232,1);check(!xband_cable_connected()); // Cached clock validation.
        xband_cable_reset();check(xband_cable_receive(0,10000)==-1);xband_cable_stop();
    }
    _putenv_s("XBAND_MEDNAFEN_CABLE_BACKEND","tcp");_putenv_s("XBAND_MEDNAFEN_CABLE_QUANTUM","4096");
    _putenv_s("XBAND_MEDNAFEN_CABLE","0");
    std::cout<<"C ABI opt-in, modem exclusion, validation, dual SCI, reset and quit cancellation passed\n";
}
