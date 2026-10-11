#include "sci_state.hpp"
#include <deque>
#include <stdexcept>
#include <iostream>
static void check(bool ok){if(!ok)throw std::runtime_error("SCI contract failed");}
int main(){
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
