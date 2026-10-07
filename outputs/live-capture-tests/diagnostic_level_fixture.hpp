#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <optional>
#include <span>
#include <cstdio>
#include <xband/local_phone_policy.hpp>
namespace diagnostic {
// Synthetic display-only title: deliberately not attributed to historical VF.
inline std::optional<uint32_t> receivedGameID(std::span<const uint8_t> request) {
    if(request.size()<23)return {};
    try {
        const auto card=xband::registrationOffset(request,135);
        if(request.size()<card+8||request[card]!=0x1e||request[card+1]!=0)return {};
        uint32_t length=0;
        for(size_t i=card+4;i<card+8;++i)length=(length<<8)|request[i];
        if(length!=0&&length!=13)return {};
        const auto pos=card+8+length;
        if(request.size()<pos+9||request[pos]!=0x0c)return {};
        uint32_t id=0;
        for(size_t i=pos+1;i<pos+5;++i)id=(id<<8)|request[i];
        return id;
    }catch(const std::runtime_error&){return {};}
}
inline std::vector<uint8_t> levelFixtureWire(std::optional<uint32_t> displayedGameID=0x00010003u) {
    char label[20]="ID UNKNOWN";
    if(displayedGameID)std::snprintf(label,sizeof(label),"ID 0x%08X",unsigned(*displayedGameID));
    std::vector<uint8_t> result;
    for(uint8_t user: {uint8_t(0),uint8_t(3)}) {
        std::vector<uint8_t> body{0x54,0x45,0x53,0x54,user,0}; // gameID TEST
        // Only the visible title changes; retain TEST key to avoid enabling
        // unverified postmatch point updates for the actual game.
        for(const auto &s: {static_cast<const char*>(label),"LEVEL 1","100","LEVEL 2","50"}) {
            std::string text=s;body.insert(body.end(),text.begin(),text.end());body.push_back(0);
        }
        const uint16_t id=uint16_t(user)*64+60;
        result.insert(result.end(),{0x25,uint8_t(id>>8),uint8_t(id),0,uint8_t(body.size())});
        result.insert(result.end(),body.begin(),body.end());
        result.insert(result.end(),4,0); // Required BE32 auxiliaryLength=0.
    }
    return result;
}
inline bool parseLevelFixture(const char *value) {
    if(!value||!*value)return false;
    if(std::string(value)!="DISPLAY_TEST")throw std::runtime_error("XBAND_LEVEL_FIXTURE must be DISPLAY_TEST");
    return true;
}
}
