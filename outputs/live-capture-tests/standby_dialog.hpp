#pragma once
#include "game_ranking_settings.hpp"
#include "standby_wait_settings.hpp"
namespace diagnostic {
// Local policy, not a reconstruction of the original population-based estimate.
// Defaults preserve the former rounded durations (1/2/4 minutes). Settings
// are server-wide; the ROM sends only the selected index, not a duration.
inline uint32_t vfStandbyWaitTicks(uint8_t preference){
    return activeStandbyWait?activeStandbyWait->ticks(preference):standbyWaitTicks(preference);
}
inline std::vector<uint8_t> standbyDialogWire(const std::string& title,uint32_t ticks){
    if(title.empty()||title.size()>64||ticks==0||ticks>standbyWaitMaxMinutes*standbyWaitTicksPerMinute)
        throw std::runtime_error("Invalid bounded standby dialog");
    const auto minutes=(ticks+3599)/3600;
    const auto text=rankingStandardEUC(rankingUTF8(L"\u3042\u306a\u305f\u306b\u3075\u3055\u308f\u3057\u3044"))+rankingEUC(title)+
        rankingStandardEUC(rankingUTF8(L"\u306e\u5bfe\u6226\u76f8\u624b\u3092\u7d04"))+std::to_string(minutes)+
        rankingStandardEUC(rankingUTF8(L"\u5206\u9593\u63a2\u3057\u307e\u3059\u3002\u3057\u3070\u3089\u304f\u304a\u5f85\u3061\u304f\u3060\u3055\u3044\u3002"));
    if(text.size()+1>256)throw std::runtime_error("Standby dialog too long");
    // Original Saturn handler0602BAEC consumes four BE16 fields then BE32 length.
    // Queue for the main menu only, not while the center connection is still up.
    // Second (template) field is consumed but not forwarded by this Saturn handler.
    std::vector<uint8_t> wire{0x22,0,1,0,0,0,120,1,44,0,0,0,0};
    const auto n=uint32_t(text.size()+1);
    for(unsigned i=0;i<4;++i)wire[9+i]=uint8_t(n>>((3-i)*8));
    wire.insert(wire.end(),text.begin(),text.end());wire.push_back(0);return wire;
}
}
