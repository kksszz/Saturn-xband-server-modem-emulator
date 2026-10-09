#pragma once
#include <array>
#include <cstdint>

namespace xband {
// Experimental model constrained by original Saturn ROM pin/read-back tests.
// Synthetic raw images only; no authentication or historical pricing claim.
// Data belongs to the card, not a user, title, server ledger, or save state.
class VirtualMediaCard {
public:
    std::array<uint8_t,13> data{};
    bool present=false;
    unsigned cursor=0,programmed=0;
    uint8_t output=0;
    unsigned resetPhase=0;
    bool programmedSinceReset=false;
    void setInserted(bool value) noexcept {present=value;resetPins();}
    void resetPins() noexcept {cursor=programmed=resetPhase=0;output=0;programmedSinceReset=false;}
    uint8_t read(uint8_t fallback=0)const noexcept {
        const auto bit=cursor<104?((data[cursor/8]>>(7-cursor%8))&1u):1u;
        return uint8_t((fallback&~0x11u)|(present?0x10u|bit:0u));
    }
    void write(uint8_t value) noexcept {
        if(!present){output=value;return;}
        if(value==0x8d){cursor=0;resetPhase=1;programmedSinceReset=false;}
        else if(resetPhase){if(value==0x81)resetPhase=0;}
        else{
            if((output&8)&&!(value&8)){
                if(programmedSinceReset&&cursor>0&&cursor<=96){
                    // Original51E4 borrow: second program pulse restores following byte.
                    data[(cursor-1)/8+1]=0xff;
                }else if(cursor<104){data[cursor/8]&=uint8_t(~(0x80u>>(cursor%8)));}
                programmedSinceReset=true;++programmed;
            }
            if((output&4)&&!(value&4)&&cursor<105)++cursor;
        }
        output=value;
    }
};
}
