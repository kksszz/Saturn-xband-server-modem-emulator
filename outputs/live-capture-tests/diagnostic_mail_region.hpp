#pragma once
#include "jp_area_code_table.hpp"
#include "game_ranking_settings.hpp"
namespace diagnostic {
// Local allocation-region policy, not an authenticated postal address.
// Unknown/ambiguous numbers have a blank address, never the sender's name.
inline std::vector<uint8_t> mailSenderAddress(const JapanAreaCodeTable& table,const std::string& phone){
    const auto region=table.lookup(phone);
    if(region.display.empty())return {};
    const auto text=rankingEUC(region.display);
    if(text.empty()||text.size()>33||text.find('\0')!=std::string::npos)
        throw std::runtime_error("Invalid mail sender region");
    return {text.begin(),text.end()};
}
}
