#pragma once
#include "bus_binding.hpp"
#include <xband/modem_register_bank.hpp>
#include <functional>
#include <stdexcept>
#include <utility>

namespace xband::ymir_adapter {
// One attachment per Saturn, installed before CPU execution. All callbacks run
// on the emulation owner thread and must not wait for network/UI work.
// Declare its owner before Saturn so the attachment outlives the mapped bus.
// Disabling forwards accesses; it does not undo other components' mappings.
class BoardAttachment final : public DeviceBus {
public:
    struct Hooks {
        std::function<ModemRegisterBank::BoardMode()> mode;
        std::function<uint8_t(uint32_t)> uartRead;
        std::function<void(uint32_t,uint8_t)> uartWrite;
        // Optional card overlay and diagnostics. Debug accesses bypass all hooks.
        std::function<uint32_t(uint32_t,unsigned,uint32_t)> overlayRead;
        // Return true only when an optional device owns this write.
        std::function<bool(uint32_t,unsigned,uint32_t)> overlayWrite;
        std::function<void(uint32_t,unsigned,uint32_t)> observedRead;
        std::function<void(uint32_t,unsigned,uint32_t)> observedWrite;
    };
private:
    ModemRegisterBank &registers;
    Hooks hooks;
    bool enabled=true;
    std::unique_ptr<BusBinding> binding;

    uint32_t fallbackRead(uint32_t a,unsigned width,bool peek) const {
        const auto &bus=binding->fallback();
        switch(width){
        case 1:return peek?bus.Peek<uint8>(a):bus.Read<uint8>(a);
        case 2:return peek?bus.Peek<uint16>(a):bus.Read<uint16>(a);
        case 4:return peek?bus.Peek<uint32>(a):bus.Read<uint32>(a);
        default:throw std::invalid_argument("modem board read width");
        }
    }
    void fallbackWrite(uint32_t a,uint32_t v,unsigned width,bool poke) const {
        switch(width){
        case 1:binding->fallbackWrite<uint8>(a,uint8(v),poke);break;
        case 2:binding->fallbackWrite<uint16>(a,uint16(v),poke);break;
        case 4:binding->fallbackWrite<uint32>(a,v,poke);break;
        default:throw std::invalid_argument("modem board write width");
        }
    }
public:
    BoardAttachment(ymir::sys::SH2Bus &bus,ModemRegisterBank &bank,Hooks callbacks)
        :registers(bank),hooks(std::move(callbacks)) {
        if(!hooks.mode||!hooks.uartRead||!hooks.uartWrite)
            throw std::invalid_argument("modem board requires mode and UART callbacks");
        binding=std::make_unique<BusBinding>(bus,*this);
    }
    BoardAttachment(const BoardAttachment&)=delete;
    BoardAttachment& operator=(const BoardAttachment&)=delete;
    // Owner-thread only, with CPU stopped at a safe boundary. Session shutdown
    // and transport cancellation belong to the owner, not the memory mapping.
    void setEnabled(bool value) noexcept {enabled=value;}
    uint32_t read(uint32_t a,unsigned width,bool peek) override {
        auto value=fallbackRead(a,width,peek);
        if(peek||!enabled)return value;
        if(hooks.overlayRead)value=hooks.overlayRead(a,width,value);
        value=registers.readBoard(a,width,value,hooks.mode(),hooks.uartRead);
        if(hooks.observedRead)hooks.observedRead(a,width,value);
        return value;
    }
    void write(uint32_t a,uint32_t value,unsigned width,bool poke) override {
        if(width!=1&&width!=2&&width!=4)throw std::invalid_argument("modem board write width");
        if(poke||!enabled){fallbackWrite(a,value,width,poke);return;}
        if(hooks.observedWrite)hooks.observedWrite(a,width,value);
        if(hooks.overlayWrite&&hooks.overlayWrite(a,width,value))return;
        if(!registers.writeBoard(a,value,width,hooks.mode(),hooks.uartWrite))
            fallbackWrite(a,value,width,false);
    }
};
}
