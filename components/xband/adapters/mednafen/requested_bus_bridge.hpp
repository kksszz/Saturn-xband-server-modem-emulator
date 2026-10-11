#pragma once
#include <xband/modem_register_bank.hpp>
#include <cstdint>
#include <functional>

namespace xband { namespace mednafen_probe {
// Prototype for the original requested access, BEFORE Mednafen's A-bus
// 32->16 splitting. No timing/network work here. Host retains its timing,
// DMA and fallback paths. Also used by the opt-in offline recognition adapter.
class RequestedBusBridge {
    ModemRegisterBank& bank;
public:
    ModemRegisterBank::BoardMode mode{};
    std::function<uint8_t(uint32_t)> uartRead;
    std::function<void(uint32_t,uint8_t)> uartWrite;
    std::function<uint8_t(uint8_t)> cardRead;
    std::function<void(uint8_t)> cardWrite;
    explicit RequestedBusBridge(ModemRegisterBank& registers):bank(registers){}
    static bool aligned(uint32_t address,unsigned width) {
        return (width==1||width==2||width==4)&&!(address&(width-1));
    }
    bool ownsRead(uint32_t address,unsigned width,bool debug=false) const {
        if(debug||!aligned(address,width))return false;
        const bool byteRegister=width==1&&
            ((address==0x05885025&&cardRead)||
             (mode.identify&&address==0x05885029)||
             (mode.uart&&ModemRegisterBank::uartAddress(address)&&uartRead)||
             (mode.flash&&bank.flashIdentify&&(address==0x04000000||address==0x04000001)));
        const bool storage=mode.storage&&address>=0x04000000&&address<=0x04020000-width;
        return byteRegister||storage;
    }
    bool ownsWrite(uint32_t address,unsigned width,bool debug=false) const {
        if(debug||!aligned(address,width))return false;
        return (mode.flash&&width==1&&address>=0x04000000&&address<0x04020000)||
            (width==1&&address==0x05885021&&bool(cardWrite))||
            (mode.uart&&width==1&&ModemRegisterBank::uartAddress(address)&&bool(uartWrite));
    }
    bool read(uint32_t address,unsigned width,uint32_t& dataBus,bool debug=false) {
        if(!ownsRead(address,width,debug))return false;
        const unsigned shift=((address&3)^(4-width))*8;
        const uint32_t mask=width==4?0xffffffffu:((1u<<(width*8))-1);
        const uint32_t fallback=(dataBus>>shift)&mask;
        const auto value=address==0x05885025&&width==1&&cardRead?cardRead(uint8_t(fallback)):bank.readBoard(address,width,fallback,mode,
            [&](uint32_t a){return uartRead?uartRead(a):uint8_t(0xff);});
        dataBus=(dataBus&~(mask<<shift))|((value&mask)<<shift);
        return true;
    }
    bool write(uint32_t address,unsigned width,uint32_t dataBus,bool debug=false) {
        if(!ownsWrite(address,width,debug))return false;
        const unsigned shift=((address&3)^(4-width))*8;
        if(address==0x05885021&&width==1&&cardWrite){cardWrite(uint8_t(dataBus>>shift));return true;}
        return bank.writeBoard(address,dataBus>>shift,width,mode,
            [&](uint32_t a,uint8_t v){if(uartWrite)uartWrite(a,v);});
    }
};
} }
