#pragma once
#include <xband/local_phone_policy.hpp>
#include <cstdint>
#include <string_view>

namespace xband {
// Read-only probe for the shared SegaNet XOS image (historical VF API name).
// A number match alone is NOT sufficient: the live call must originate from
// 0609DBC0 (phone-setting self-test), not the ordinary peer dial path.
template<class PeekWord, class PeekLong, class PeekByte>
bool vfPhoneCheckActive(std::string_view number, uint32_t stack,
                       PeekWord word, PeekLong value, PeekByte byte) {
    const auto digits=phoneDigits(number);
    if(digits.empty() || digits=="0120717360")return false;
    if(word(0x0609dbc0)!=0x2f86 || word(0x0609dbe0)!=0x400b ||
       value(0x0609dc28)!=0x060ba854 || value(0x0609dc34)!=0x0609e0fc ||
       word(0x0609e0fc)!=0x2fe6 || word(0x0609e0fe)!=0x4f22 ||
       value(0x0609e114)!=0x00530009 ||
       value(0x060294b8)!=0x2f862f96 || value(0x060294bc)!=0x2fe64f22)
        return false;
    // The self-test uses the local number, excluding the area code. Read only
    // the bounded NUL-terminated field currently edited by the guest.
    std::string own;
    for(uint32_t i=0;i<32;++i){
        const auto c=byte(0x060ba854+i);
        if(!c)break;
        own+=char(c);
    }
    if(own.empty() || byte(0x060ba854+uint32_t(own.size()))!=0 ||
       phoneDigits(own)!=digits)return false;
    // Ignore stale stack contents below SP. Require the two nested frames and
    // their saved frame pointers, not an isolated return-address occurrence.
    stack&=0x1fffffff;
    if((stack&3) || stack<0x06000000 || stack>0x060ffff0)return false;
    const auto limit=stack>0x060ffdf0?0x060ffff0:stack+512;
    for(uint32_t at=stack+16;at<=limit;at+=4){
        if(value(at)==0x0609dbe4 && (value(at+4)&0x1fffffff)==at+8 &&
           value(at-16)==0x0609e10a && (value(at-12)&0x1fffffff)==at)
            return true;
    }
    return false;
}
}
