#pragma once
#include <xband/device_bus.hpp>
#include <ymir/sys/bus.hpp>
#include <memory>
#include <array>

namespace xband::ymir_adapter {
// Thin Ymir-specific mapping only: no socket, PPP, game identity or card store.
// The endpoint must forward unowned addresses to fallback(). CS2 also contains
// the CD block; mapping that whole window as an exclusive modem is incorrect.
// Keep alive until the guest bus is destroyed. Install only while stopped.
class BusBinding final {
    ymir::sys::SH2Bus &bus;
    DeviceBus &device;
    std::unique_ptr<ymir::sys::SH2Bus> saved;
    static constexpr std::array<std::array<uint32_t,2>,2> ranges{{
        {0x02000000,0x04ffffff},{0x05800000,0x058fffff}}};
    template<class T,bool debug=false> static T read(uint32 a,void *ctx){
        auto &self=*static_cast<BusBinding*>(ctx);
        return static_cast<T>(self.device.read(a,sizeof(T),debug));
    }
    template<class T,bool debug=false> static void write(uint32 a,T v,void *ctx){
        static_cast<BusBinding*>(ctx)->device.write(a,v,sizeof(T),debug);
    }
    void map(){
        for(const auto &r:ranges){
                bus.MapNormal(r[0],r[1],this,read<uint8>,read<uint16>,read<uint32>,
                    write<uint8>,write<uint16>,write<uint32>,
                    [](uint32 a,uint32 n,bool w,void*c){return static_cast<BusBinding*>(c)->saved->IsBusWait(a,n,w);});
                bus.MapSideEffectFree(r[0],r[1],this,read<uint8,true>,read<uint16,true>,read<uint32,true>,
                    write<uint8,true>,write<uint16,true>,write<uint32,true>);
        }
    }
public:
    BusBinding(ymir::sys::SH2Bus &b,DeviceBus &d):bus(b),device(d),saved(std::make_unique<ymir::sys::SH2Bus>(b)){map();}
    BusBinding(const BusBinding&)=delete;
    BusBinding& operator=(const BusBinding&)=delete;
    // No automatic restore: the host owns mapping lifetime and may have added
    // other overlays. Keep this binding alive until the guest bus is destroyed.
    const ymir::sys::SH2Bus &fallback()const{return *saved;}
    // Ymir's write dispatch is non-const even when mapping itself is unchanged.
    template<class T> void fallbackWrite(uint32 address,T value,bool debug){
        if(debug)saved->Poke<T>(address,value);else saved->Write<T>(address,value);
    }
};
}
