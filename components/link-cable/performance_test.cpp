// SPDX-License-Identifier: GPL-3.0-only
// Two-process microbenchmark, not game FPS. Owner polling at 64-cycle intervals,
// no BIOS/disc/profile. Configure each process via the cable environment.
#include "cable_runtime.h"
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <thread>
static auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
static int cancelled(){return std::chrono::steady_clock::now()>deadline;}
int main(){
    if(!xband_cable_start(0))return 1;
    xband_cable_set_cancel(cancelled);
    while(!xband_cable_connected()&&!cancelled()){
        xband_cable_budget(0,0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(cancelled()){xband_cable_stop();return 2;}
    constexpr std::uint64_t total=16777216,step=64;
    const char* byteSetting=std::getenv("SATURN_LINK_BENCH_BYTES");
    const bool verifyBytes=byteSetting&&byteSetting[0]=='1'&&byteSetting[1]=='\0';
    unsigned received[2]={0,0};
    const auto start=std::chrono::steady_clock::now();
    for(std::uint64_t cycle=0;cycle<total&&!cancelled();cycle+=step){
        xband_cable_budget(cycle,0);
        if(!xband_cable_connected()){xband_cable_stop();return 3;}
        if(verifyBytes&&cycle==64){
            for(unsigned i=0;i<512;++i)for(unsigned master=0;master<2;++master)
                xband_cable_send(master,std::uint8_t(i+master),cycle);
        }
        for(unsigned master=0;master<2;++master){
            int value;
            while((value=xband_cable_receive(master,cycle))>=0){
                if(!verifyBytes||received[master]>=512||
                   value!=int(std::uint8_t(received[master]+master))){xband_cable_stop();return 4;}
                ++received[master];
            }
        }
    }
    xband_cable_budget(total,0);
    if(verifyBytes&&(received[0]!=512||received[1]!=512)){xband_cable_stop();return 6;}
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("cycles=%llu polls_per_lane=%llu wall_seconds=%.6f bytes_slave=%u bytes_master=%u\n",
        static_cast<unsigned long long>(total),static_cast<unsigned long long>(total/step),seconds,received[0],received[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    xband_cable_stop();return cancelled()?5:0;
}
