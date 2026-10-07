#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
// One observed PB3 layout only. Synthetic fixture, not an authenticated peer.
inline std::vector<uint8_t> diagnosticPeerProfile(const std::vector<uint8_t>& source) {
    using B=std::vector<uint8_t>;
    constexpr unsigned lengths[]={4,80,24,28,4,4,4,0,0,0};
    if(source.size()!=188)throw std::runtime_error("profile fixture requires 188 bytes");
    size_t cursor=0;
    for(unsigned i=0;i<10;++i) {
        const uint32_t n=(uint32_t(source[cursor])<<24)|(uint32_t(source[cursor+1])<<16)|
                         (uint32_t(source[cursor+2])<<8)|source[cursor+3];
        if(n!=lengths[i])throw std::runtime_error("profile fixture layout mismatch");
        cursor+=4;
        if(i>=2&&i<=4&&std::find(source.begin()+cursor,source.begin()+cursor+n,0)==source.begin()+cursor+n)
            throw std::runtime_error("profile fixture missing string terminator");
        cursor+=n;
    }
    B result=source;
    const char key[]="TESTPEER";
    std::copy(key,key+8,result.begin()+12);result[20]=1;
    // Preserve the entire remaining fixed block: its string capacities are unproven.
    auto message=[&](size_t start,size_t size,const char* text) {
        std::fill(result.begin()+start,result.begin()+start+size,0);
        for(size_t i=0;text[i];++i) {
            if(i+1>=size)throw std::runtime_error("fixture message too long");
            result[start+i]=uint8_t(text[i]);
        }
    };
    message(96,24,"LOCAL FIXTURE ONLY");
    message(124,28,"NOT A REAL XBAND PEER");
    return result;
}
