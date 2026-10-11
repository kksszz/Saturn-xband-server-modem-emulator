#include "link_cable_service.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
using saturn::link::LinkCableService;
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>static void until(F test){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);while(!test()){check(std::chrono::steady_clock::now()<end,"TCP deadline");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
static void run(){
    LinkCableService a,b;
    a.SetDualSCI(true);b.SetDualSCI(true);
    // Private regression port; no XBAND service or user's cable port.
    a.Listen(59378);b.Connect("127.0.0.1",59378);
    until([&]{return a.GetState()==LinkCableService::State::Connected&&b.GetState()==LinkCableService::State::Connected;});
    for(unsigned i=0;i<1000;++i){a.TransmitSCI(true,i%256,0);a.TransmitSCI(false,(i+1)%256,0);b.TransmitSCI(true,(i+2)%256,0);b.TransmitSCI(false,(i+3)%256,0);}
    for(unsigned i=0;i<1000;++i){
        const auto take=[&](LinkCableService& e,bool master,unsigned value){std::optional<saturn::link::uint8> v;until([&]{v=e.ReceiveSCI(master,0);return v.has_value();});check(*v==value,"lane/order/data");};
        take(b,true,i%256);take(b,false,(i+1)%256);take(a,true,(i+2)%256);take(a,false,(i+3)%256);
    }
    a.Disconnect();b.Disconnect();check(!a.ReceiveSCI(true,0),"stale data");
    a.SetSynchronized(true);b.SetSynchronized(true);
    a.Listen(59379);b.Connect("127.0.0.1",59379);
    until([&]{return a.GetState()==LinkCableService::State::Connected&&b.GetState()==LinkCableService::State::Connected;});
    check(a.AdvanceSynchronized(100,1,1,[]{return false;})==4096,"origin");
    check(b.AdvanceSynchronized(900,1,1,[]{return false;})==4096,"independent origin");
    a.TransmitSCI(true,77,110);
    check(!b.ReceiveSCI(true,900),"virtual latency");
    auto fa=std::async(std::launch::async,[&]{return a.AdvanceSynchronized(4196,1,1,[]{return false;});});
    auto fb=std::async(std::launch::async,[&]{return b.AdvanceSynchronized(4996,1,1,[]{return false;});});
    check(fa.wait_for(std::chrono::seconds(3))==std::future_status::ready,"barrier A");check(fb.wait_for(std::chrono::seconds(3))==std::future_status::ready,"barrier B");
    check(fa.get()==4096&&fb.get()==4096,"barrier budget");
    std::optional<saturn::link::uint8> v;until([&]{v=b.ReceiveSCI(true,5006);return v.has_value();});check(*v==77,"timed delivery");
    a.AdvanceSynchronized(4196,2,1,[]{return false;});check(a.GetState()==LinkCableService::State::Error,"revision invalidates");
    a.Disconnect();b.Disconnect();
    std::cout<<"4000 lane bytes, independent origins, barrier, latency and reset passed\n";
}
int main(){try{run();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
