// SPDX-License-Identifier: GPL-3.0-only
// Host-independent byte-level SH-2 SCI, derived from this project's YMIR SCI.
// C++11 compatible. It does not emulate external clock pins or serial waveforms.
#pragma once
#include <cstdint>

namespace saturn { namespace link {
struct SciState {
    enum { TDRE=0x80, RDRF=0x40, ORER=0x20, TEND=0x04, TE=0x20, RE=0x10 };
    std::uint8_t SMR=0, BRR=255, SCR=0, TDR=255, SSR=TDRE|TEND, RDR=0;
    std::uint8_t observed=0, txValue=0;
    std::uint64_t txComplete=0, rxReady=0;
    bool txBusy=false;
    void reset() { *this=SciState(); }
    std::uint64_t frameCycles() const {
        const std::uint64_t divider=std::uint64_t(1)<<(2*(SMR&3));
        const unsigned data=(SMR&64)?7:8;
        const unsigned bits=(SMR&128)?data:1+data+((SMR&32)?1:0)+((SMR&8)?2:1);
        return ((SMR&128)?8:32)*std::uint64_t(BRR+1)*divider*bits;
    }
    void start(std::uint8_t value,std::uint64_t now) {
        TDR=value; SSR&=~(TDRE|TEND); observed&=~TDRE;
        if(!txBusy) {
            txValue=value; txComplete=now+frameCycles(); txBusy=true; SSR|=TDRE;
        }
    }
    std::uint8_t read(unsigned reg,bool peek=false) {
        switch(reg) {
        case 0:return SMR; case 1:return BRR; case 2:return SCR; case 3:return TDR;
        case 4:if(!peek)observed|=SSR;return SSR; case 5:return RDR;
        default:return 255;
        }
    }
    void write(unsigned reg,std::uint8_t value,std::uint64_t now) {
        switch(reg) {
        case 0:SMR=value;break;
        case 1:BRR=value;break;
        case 2:{
            const bool wasTE=SCR&TE, wasRE=SCR&RE; SCR=value;
            if(!(SCR&TE)) { txBusy=false; SSR|=TDRE|TEND; }
            else if(!wasTE && !(SSR&TDRE))start(TDR,now);
            if(!wasRE && (SCR&RE))rxReady=0;
            break;
        }
        case 3:TDR=value;break;
        case 4:{
            const std::uint8_t clear=SSR&observed&~value&0xf8;
            SSR&=~clear;observed&=~clear;
            if((clear&TDRE)&&(SCR&TE))start(TDR,now);
            break;
        }
        default:break; // RDR is read-only.
        }
    }
    bool receive(std::uint8_t value,std::uint64_t now) {
        if(!(SCR&RE))return false;
        if(SSR&RDRF){SSR|=ORER;return false;}
        RDR=value;SSR|=RDRF;rxReady=now+frameCycles();return true;
    }
    template<class Send,class Pop> void tick(std::uint64_t now,Send send,Pop pop) {
        while(txBusy && now>=txComplete) {
            send(txValue,now);
            if(!(SSR&TDRE)){txValue=TDR;txComplete+=frameCycles();SSR|=TDRE;}
            else {txBusy=false;SSR|=TEND;}
        }
        if(!(SCR&RE)) {
            for(unsigned i=0;i<65536;++i)if(pop(now)<0)break;
        } else if(!(SSR&RDRF) && now>=rxReady) {
            const int value=pop(now);if(value>=0)receive(std::uint8_t(value),now);
        }
    }
    // SH-2 DRCR module request: 1=SCI RXI, 2=SCI TXI.
    bool dmaReady(unsigned request) const {
        return request==1?((SCR&RE)&&(SSR&RDRF)):
               request==2?((SCR&TE)&&(SSR&TDRE)):false;
    }
    std::uint8_t dmaRead() {const auto v=RDR;SSR&=~RDRF;observed&=~RDRF;return v;}
    void dmaWrite(std::uint8_t value,std::uint64_t now){if(SCR&TE)start(value,now);}
    // 0=no interrupt, 1=ERI, 2=RXI, 3=TXI, 4=TEI, in hardware priority order.
    unsigned interrupt() const {
        if((SCR&64)&&(SSR&0x38))return 1;
        if((SCR&64)&&(SSR&RDRF))return 2;
        if((SCR&128)&&(SSR&TDRE))return 3;
        if((SCR&4)&&(SSR&TEND))return 4;
        return 0;
    }
};
}} // namespace saturn::link
