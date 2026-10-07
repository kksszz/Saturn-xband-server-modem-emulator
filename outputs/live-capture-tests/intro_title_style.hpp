#pragma once
#include <cstdint>
#include <optional>
#include <vector>
namespace diagnostic {
// Verified original DRAMDB0206:0000: title callback9, width200, height20.
// VF-only experiment. Resource updates persist; use copied VF test profiles.
inline std::vector<uint8_t> introTitleOriginalStyle(){
    return {0,6,0,1,0,0,0,0,0,200,0,20,0,1,0x11,0,
        3,0x39,0,0,0,4,0,0,0,0,0,7,0,0,0,9,0};
}
inline std::vector<uint8_t> introTitleCompactStyleWire(std::optional<uint32_t> game){
    if(!game||*game!=0x00010003)return {};
    auto body=introTitleOriginalStyle();body[14]=0x15;
    // Original opcode12 receives type:u16, ID:u16, length:u32, then raw bytes.
    // Change font1100 to1500 only; geometry, colors and callback stay original.
    std::vector<uint8_t> wire{0x12,2,6,0,0,0,0,0,33};
    wire.insert(wire.end(),body.begin(),body.end());return wire;
}
}
