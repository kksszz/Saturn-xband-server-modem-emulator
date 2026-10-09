#pragma once
#include "game_ranking_settings.hpp"
#include <optional>
namespace diagnostic {
inline std::vector<uint8_t> creditNoticeReply(const std::wstring& message){
    if(message.empty()||message.size()>128||message.find(L'\0')!=std::wstring::npos)
        throw std::runtime_error("Invalid bounded credit notice");
    std::string text;
    // The shared converter limits individual fields to64 encoded bytes.
    // Short chunks preserve that bound without truncating the notification.
    for(size_t at=0;at<message.size();at+=16)
        text+=rankingStandardEUC(rankingUTF8(message.substr(at,16)));
    if(text.size()+1>256)throw std::runtime_error("Credit notice too long");
    std::vector<uint8_t> wire{0x22,0,1,0,0,0,120,1,44,0,0,0,0};
    const auto length=uint32_t(text.size()+1);
    for(unsigned i=0;i<4;++i)wire[9+i]=uint8_t(length>>((3-i)*8));
    wire.insert(wire.end(),text.begin(),text.end());wire.push_back(0);wire.push_back(2);
    return wire;
}
// Server-local wording using the verified Saturn22 notification and02
// terminator. This is NOT a reconstructed historical -505 wire error ABI.
inline std::vector<uint8_t> creditInsufficientReply(uint32_t requested,int32_t remaining,
                                                   std::optional<int64_t> consumed={}){
    if(requested>32767||remaining<0||remaining>32767||
       (consumed&&(*consumed<0||*consumed>requested)))
        throw std::runtime_error("Invalid bounded insufficient-credit notice");
    // The shared field converter is bounded to64 bytes per call. Convert the
    // short text fragments separately, then bound the whole notification.
    auto encode=[](const wchar_t* text){return rankingStandardEUC(rankingUTF8(text));};
    auto text=encode(L"クレジットが不足しています。");
    if(consumed)text+=encode(L"要求")+std::to_string(requested)+encode(L"、消費")+
        std::to_string(*consumed)+encode(L"、残り")+std::to_string(remaining)+encode(L"度数です。");
    else text+=encode(L"必要")+std::to_string(requested)+encode(L"、残り")+
        std::to_string(remaining)+encode(L"度数です。");
    text+=encode(L"メールの送受信・対戦接続は行いません。");
    if(text.size()+1>256)throw std::runtime_error("Insufficient-credit notice too long");
    std::vector<uint8_t> wire{0x22,0,1,0,0,0,120,1,44,0,0,0,0};
    const auto length=uint32_t(text.size()+1);
    for(unsigned i=0;i<4;++i)wire[9+i]=uint8_t(length>>((3-i)*8));
    wire.insert(wire.end(),text.begin(),text.end());wire.push_back(0);wire.push_back(2);
    return wire;
}
}
