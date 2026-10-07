#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <ostream>

namespace xband {
// Extracted from the successful Saturn replay. This is the observed register
// subset, not a complete 16550/flash specification. No emulator, network, AT
// parser, file, card ledger, GUI or guest-memory dependency.
class ModemRegisterBank {
public:
    struct BoardMode {bool identify=false,uart=false,flash=false,storage=false;};
    uint8_t dll=0,dlm=0,ier=0,lcr=0,mcr=0,scr=1,fcr=0;
    std::deque<uint8_t> rx;
    unsigned rxConsumed=0,rxRejected=0;
    std::array<uint8_t,0x20000> flashData=[] {
        std::array<uint8_t,0x20000> data;data.fill(0xff);return data;
    }();
    unsigned programBytes=0,flashUnlock=0;
    bool flashIdentify=false;

    // Raw diagnostic copy, not a bus read: identification/program mode and
    // UART/FIFO state must remain untouched. Caller owns thread synchronization.
    void dumpFlash(std::ostream &out) const {
        out.write(reinterpret_cast<const char *>(flashData.data()),
                  static_cast<std::streamsize>(flashData.size()));
    }

    // Reconnecting a network transport is not replacing the cartridge.
    // Keep ROM-programmed flash (phone/profile/mail resources) intact.
    void resetUART() {
        dll=dlm=ier=lcr=mcr=fcr=0;scr=1;
        rx.clear();rxConsumed=rxRejected=0;
    }

    static bool uartAddress(uint32_t address) noexcept {
        return address>=0x05895001&&address<=0x0589501d&&(address&3)==1;
    }
    // The emulator adapter owns fallback/debug semantics and shared CS2/CD
    // mapping. Values are in the Saturn big-endian bus order, not host order.
    template<class UartRead>
    uint32_t readBoard(uint32_t address,unsigned width,uint32_t fallback,
                       BoardMode mode,UartRead readUart){
        if(width!=1&&width!=2&&width!=4)throw std::invalid_argument("modem bus width");
        auto value=fallback;
        if(mode.identify&&address==0x05885029&&width==1)value=0x11;
        if(mode.uart&&width==1&&uartAddress(address))value=readUart(address);
        if(mode.storage&&width<=4&&address>=0x04000000&&address<=0x04020000-width){
            value=0;
            for(unsigned i=0;i<width;++i)value=(value<<8)|flashData[address-0x04000000+i];
        }
        if(mode.flash&&flashIdentify&&width==1){
            if(address==0x04000000)value=0x1f;
            if(address==0x04000001)value=0xd5;
        }
        return value;
    }
    template<class UartWrite>
    bool writeBoard(uint32_t address,uint32_t value,unsigned width,BoardMode mode,UartWrite writeUart){
        if(width!=1&&width!=2&&width!=4)throw std::invalid_argument("modem bus width");
        if(mode.flash&&width==1&&address>=0x04000000&&address<0x04020000){
            programFlash(address,static_cast<uint8_t>(value),mode.storage);return true;
        }
        if(mode.uart&&width==1&&uartAddress(address)){
            writeUart(address,static_cast<uint8_t>(value));return true;
        }
        return false; // caller must forward to the original bus exactly once
    }
    void programFlash(uint32_t address,uint8_t value,bool storageEnabled){
        if(address<0x04000000||address>=0x04020000)return;
        const auto offset=address-0x04000000;
        if(storageEnabled&&programBytes){flashData[offset]=value;--programBytes;return;}
        if(value==0xf0){flashIdentify=false;flashUnlock=0;return;}
        if(flashUnlock==0)flashUnlock=(offset==0x5555&&value==0xaa)?1:0;
        else if(flashUnlock==1)flashUnlock=(offset==0x2aaa&&value==0x55)?2:0;
        else{
            flashIdentify=offset==0x5555&&value==0x90;
            if(storageEnabled&&offset==0x5555&&value==0xa0)programBytes=128;
            flashUnlock=0;
        }
    }
    // Owner pumps its modem before calling. onReceive is notification only;
    // it must not perform network I/O or change the guest execution timeline.
    template<class Received>
    uint8_t readRegister(uint32_t address,bool responses,uint8_t interrupt,
                         bool carrier,uint8_t txStatus,Received onReceive){
        switch((address>>2)&7){
        case 0:
            if(lcr&0x80)return dll;
            if(responses&&!rx.empty()){
                const auto value=rx.front();rx.pop_front();++rxConsumed;
                onReceive(value);return value;
            }
            return 0;
        case 1:return lcr&0x80?dlm:ier;
        case 2:return ((fcr&1)?0xc0:0)|interrupt;
        case 3:return lcr;
        case 4:return mcr;
        case 5:return txStatus|((responses&&!rx.empty())?1:0);
        case 6:return 0x30|(carrier?0x80:0);
        default:return scr;
        }
    }
    template<class Transmit,class RxActivity,class ClearTransmit>
    void writeRegister(uint32_t address,uint8_t value,bool responses,
                       Transmit transmit,RxActivity activity,ClearTransmit clearTransmit){
        switch((address>>2)&7){
        case 0:if(lcr&0x80)dll=value;else transmit(value);break;
        case 1:if(lcr&0x80)dlm=value;else ier=value;break;
        case 2:fcr=value;if(responses&&(value&2))rx.clear();
            if(value&4)clearTransmit();activity();break;
        case 3:lcr=value;break;
        case 4:mcr=value;break;
        case 7:scr=value;break;
        }
    }
};
}
