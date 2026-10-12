#include "sci_state.hpp"
#include "cable_timebase.hpp"
#include <deque>
#include <stdexcept>
#include <iostream>
static void check(bool ok){if(!ok)throw std::runtime_error("SCI contract failed");}
int main(){
    saturn::link::CableTimebase normal,fast,mixed;
    normal.endFrame(6100,65);fast.endFrame(6500,61);
    check(normal.sample(0,65)==6100&&fast.sample(0,65)==6100);
    mixed.endFrame(100,65);check(mixed.sample(0,61)==100);
    mixed.endFrame(6500,61);check(mixed.sample(0,65)==6200);
    check(mixed.sample(100,65)==6300);
    saturn::link::CableTimebase fraction;
    for(unsigned i=0;i<65;++i)fraction.endFrame(1,61);
    check(fraction.sample(0,65)==61); // No rounding loss on frame rebases.
    for(unsigned startDivider:{61u,65u}){
        saturn::link::CableTimebase gated;
        unsigned divider=startDivider,samples=0;
        std::uint64_t nextRaw=0,boundary=4096,frameStart=0;
        for(std::uint64_t raw=0;raw<=20000;++raw){
            if(raw==8192){gated.endFrame(raw-frameStart,divider);frameStart=raw;divider=divider==61?65:61;}
            if(raw<nextRaw)continue;
            const auto time=gated.sample(raw-frameStart,divider);++samples;
            check(time<=boundary); // Poll never crosses a synchronization grant.
            if(time==boundary)boundary+=4096;
            const auto budget=boundary-time;
            nextRaw=raw+(budget<64?budget:64);
        }
        check(samples<400); // No fixed-clock conversion per guest instruction.
    }
    using saturn::link::SciState;
    SciState s;std::deque<int> tx,rx;
    const auto send=[&](unsigned b,std::uint64_t){tx.push_back(b);};
    const auto pop=[&](std::uint64_t)->int{if(rx.empty())return -1;int b=rx.front();rx.pop_front();return b;};
    check(s.SSR==0x84&&s.BRR==255);
    s.BRR=0;check(s.frameCycles()==320);
    s.SMR=128;check(s.frameCycles()==64);s.SMR=0;
    s.write(2,0x20,0);s.write(3,0x41,0);
    s.read(4,true);s.write(4,0,0);check(!s.txBusy); // peek is not observe.
    s.read(4);s.write(4,0,0);check(s.txBusy&&s.txComplete==320&&(s.SSR&0x80));
    s.tick(319,send,pop);check(tx.empty());
    s.write(3,0x42,319);s.read(4);s.write(4,0,319);
    s.tick(320,send,pop);check(tx.size()==1&&tx[0]==0x41&&s.txBusy&&!(s.SSR&4));
    s.tick(640,send,pop);check(tx.size()==2&&tx[1]==0x42&&!s.txBusy&&(s.SSR&4));
    s.write(2,0x50,641);rx.push_back(0xff);rx.push_back(0x00);
    s.tick(641,send,pop);check(s.RDR==255&&(s.SSR&64)&&s.interrupt()==2);
    s.tick(962,send,pop);check(rx.size()==1);
    check(s.read(5)==255&&(s.SSR&64)); // CPU RDR read does not clear.
    check(!s.receive(1,962)&&(s.SSR&32)&&s.interrupt()==1);
    s.read(4);s.write(4,0x9f,962);check(!(s.SSR&64)&&!(s.SSR&32));
    s.tick(962,send,pop);check(s.RDR==0&&s.dmaReady(1));
    check(s.dmaRead()==0&&!s.dmaReady(1));
    s.write(2,0xa4,1000);check(s.dmaReady(2)&&s.interrupt()==3);
    s.dmaWrite(0x55,1000);s.tick(1320,send,pop);check(tx.back()==0x55);
    s.write(2,0,1321);rx.push_back(123);s.tick(1321,send,pop);check(rx.empty());
    s.reset();check(s.SSR==0x84&&!s.txBusy&&s.rxReady==0);
    std::cout<<"SCI timing, observe/clear, double buffer, RX, DMA and IRQ passed\n";
}
