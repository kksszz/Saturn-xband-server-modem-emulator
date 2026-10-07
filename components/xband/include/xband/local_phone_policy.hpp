#pragma once
#include <string>
#include <string_view>
#include <span>
#include <cstdint>
#include <stdexcept>
namespace xband {
// Two-endpoint local compatibility mode, not a public telephone exchange.
enum class LocalDialRole {reject,selfCheck,service,peer};
inline std::string phoneDigits(std::string_view text){
    if(text.empty()||text.size()>32)return {};
    std::string out;
    for(char c:text){if(c=='-')continue;if(c<'0'||c>'9')return {};out+=c;}
    return out;
}
inline LocalDialRole localDialRole(std::string_view text,unsigned side,bool serviceBegun){
    const auto n=phoneDigits(text);if(n.empty()||side>1)return LocalDialRole::reject;
    if(n=="0120717360")return LocalDialRole::service;
    // Before service login the observed guest flow tests its own telephone.
    // Never apply this BUSY response to all calls after service login.
    if(!serviceBegun)return LocalDialRole::selfCheck;
    // The entered destination is not an address in local automatic-pair mode.
    return LocalDialRole::peer;
}
// Observed registration: phone length (including NUL) at 22, phone at 23.
// Subsequent fields move with that length; 12 was only the original fixture.
// Original offset115 holds a u16 receive-record count, followed by u16
// values copied from each record+54 (0602C964..C99E). The preceding two bytes
// belong to the 0x50-byte profile, NOT the count. Observed value0089 equals
// that fixture's opaqueD/record+54, not an established mail ID or receipt.
inline size_t registrationOffset(std::span<const uint8_t> b,size_t original){
    if(b.size()<23 || b[22]<2 || b[22]>33)
        throw std::runtime_error("Invalid registration phone length");
    size_t offset=original-12+b[22];
    if(original>=117){
        const size_t list=115-12+b[22];
        if(b.size()<list+2)throw std::runtime_error("Incomplete registration word list");
        const uint16_t count=uint16_t((unsigned(b[list])<<8)|b[list+1]);
        if(count>16)throw std::runtime_error("Unsupported registration word-list count");
        offset+=size_t(count)*2;
    }
    return offset;
}
inline std::string registrationPhone(std::span<const uint8_t> b){
    const auto end=registrationOffset(b,35);
    if(b.size()<end)throw std::runtime_error("Incomplete registration record");
    if(b[end-1]!=0)throw std::runtime_error("Unterminated registration phone");
    std::string phone(b.begin()+23,b.begin()+end-1);
    if(phoneDigits(phone).empty())throw std::runtime_error("Invalid registration phone field");
    return phone;
}
}
