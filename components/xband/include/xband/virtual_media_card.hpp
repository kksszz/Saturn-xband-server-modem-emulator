#pragma once
#include <array>
#include <cstdint>
#include <bit>
#include <optional>

namespace xband {
// Experimental model constrained by original Saturn ROM pin/read-back tests.
// Synthetic raw images only; no authentication or historical pricing claim.
// Data belongs to the card, not a user, title, server ledger, or save state.
class VirtualMediaCard {
public:
    std::array<uint8_t,13> data{};
    bool present=false;
    // Inserted but no readable chip response. Runtime fault simulation only;
    // never damage or replace the persisted image.
    bool readFault=false;
    unsigned cursor=0,programmed=0;
    uint8_t output=0;
    unsigned resetPhase=0;
    bool programmedSinceReset=false;
    // Shared card encoding: weighted bit counts of bytes8..12, signed word.
    // Report unsupported/unreadable data as unknown, never as zero credit.
    std::optional<int32_t> remainingUnits()const noexcept {
        if(readFault)return {};
        bool allFF=true;for(auto byte:data)allFF=allFF&&byte==0xff;
        if(allFF)return {};
        uint32_t value=0;for(unsigned i=8;i<13;++i)value=value*8+std::popcount(unsigned(data[i]));
        value&=0xffffu;if(value&0x8000u)return {};
        return int32_t(value);
    }
    void setInserted(bool value) noexcept {present=value;resetPins();}
    void setReadFault(bool value) noexcept {readFault=value;resetPins();}
    void resetPins() noexcept {cursor=programmed=resetPhase=0;output=0;programmedSinceReset=false;}
    uint8_t read(uint8_t fallback=0)const noexcept {
        const auto bit=readFault?1u:cursor<104?((data[cursor/8]>>(7-cursor%8))&1u):1u;
        return uint8_t((fallback&~0x11u)|(present?0x10u|bit:0u));
    }
    void write(uint8_t value) noexcept {
        if(!present||readFault){output=value;return;}
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
