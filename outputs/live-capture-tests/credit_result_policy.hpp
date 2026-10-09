#pragma once
#include <cstdint>

namespace diagnostic {
// One server-wide result policy; title IDs never select reset semantics.
// Game/account/card identity is checked separately before billing.
inline constexpr bool creditLocalReset(uint8_t opcode,int64_t error){
    return opcode==0x23&&error==-608;
}
inline constexpr bool creditPeerReset(uint8_t opcode,int64_t error){
    return opcode==0x23&&error==-609;
}
inline constexpr bool creditReset(uint8_t opcode,int64_t error){
    return creditLocalReset(opcode,error)||creditPeerReset(opcode,error);
}
}
