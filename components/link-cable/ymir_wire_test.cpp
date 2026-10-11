#include "link_cable_service.hpp"
// Resolve the common header locally; CMake supplies the original YMIR header
// by absolute path so both implementations can participate in this test.
#include SATURN_ORIGINAL_YMIR_LINK_HEADER
#include <future>
#include <iostream>
#include <stdexcept>
using Cable=saturn::link::LinkCableService;
using YmirCable=app::services::LinkCableService;
static void check(bool ok){if(!ok)throw std::runtime_error("YMIR YLS1 interoperability");}
template<class F>static void until(F test){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);while(!test()){check(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
int main(){
    for(int sync=0;sync<2;++sync){
        Cable a;YmirCable b;a.SetDualSCI(true);b.SetDualSCI(true);a.SetSynchronized(sync);b.SetSynchronized(sync);
        a.Listen(uint16_t(59382+sync));b.Connect("127.0.0.1",uint16_t(59382+sync));
        until([&]{return a.GetState()==Cable::State::Connected&&b.GetState()==YmirCable::State::Connected;});
        if(sync){check(a.AdvanceSynchronized(0,1,0,[]{return false;})==4096);check(b.AdvanceSynchronized(100,1,0,[]{return false;})==4096);}
        for(unsigned i=0;i<256;++i){a.TransmitSCI(true,uint8_t(i),1);a.TransmitSCI(false,uint8_t(255-i),1);b.TransmitSCI(true,uint8_t(i),101);b.TransmitSCI(false,uint8_t(255-i),101);}
        if(sync){
            auto fa=std::async(std::launch::async,[&]{return a.AdvanceSynchronized(4096,1,0,[]{return false;});});
            auto fb=std::async(std::launch::async,[&]{return b.AdvanceSynchronized(4196,1,0,[]{return false;});});
            check(fa.wait_for(std::chrono::seconds(3))==std::future_status::ready);check(fb.wait_for(std::chrono::seconds(3))==std::future_status::ready);
            check(fa.get()==4096&&fb.get()==4096);
        }
        for(unsigned i=0;i<256;++i)for(bool master:{false,true}){
            std::optional<uint8_t> va,vb;
            until([&]{if(!va)va=a.ReceiveSCI(master,4097);if(!vb)vb=b.ReceiveSCI(master,4197);return va.has_value()&&vb.has_value();});
            check(*va==(master?i:255-i)&&*vb==*va);
        }
        a.Disconnect();b.Disconnect();
    }
    std::cout<<"Original YMIR dual-SCI YLS1, sync OFF/ON, all 256 byte values passed\n";
}
