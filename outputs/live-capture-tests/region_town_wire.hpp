#pragma once
#include "jp_area_code_table.hpp"
#include "game_ranking_settings.hpp"
namespace diagnostic {
// Shared XOS command38: exactly34 bytes, NUL-terminated EUC-JP town.
// This changes the registered allocation-region text, not matchmaking policy.
inline std::vector<uint8_t> regionTownWire(const JapanAreaCodeTable& table,
                                         const std::string& phone,
                                         std::optional<uint32_t> game) {
    if(!game)return {};
    const auto region=table.lookup(phone);
    if(region.display.empty())return {}; // Preserve guest data on unknown/ambiguous lookup.
    const auto text=rankingEUC(region.display);
    if(text.empty()||text.size()>=34||text.find('\0')!=std::string::npos)
        throw std::runtime_error("Invalid region town text");
    std::vector<uint8_t> wire(35,0);wire[0]=0x38;
    std::copy(text.begin(),text.end(),wire.begin()+1);return wire;
}
}
