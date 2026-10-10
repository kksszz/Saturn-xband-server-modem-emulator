#pragma once
#include "game_ranking_settings.hpp"
#include <optional>
namespace diagnostic {
// Newly authored receive0A program, not recovered historical server traffic.
// Calls the shared Saturn XBAND type0118 display routine. No ROM text/assets
// are distributed. ABI verified against the VF Remix/Puzzle Bobble 3 engine;
// other engine revisions have not been verified. Never accept arbitrary code/ID.
inline std::vector<uint8_t> originalCardWarningReply(uint16_t id){
    if(id!=0x42&&id!=0x75&&id!=0x107)
        throw std::runtime_error("Unsupported original card warning");
    std::vector<uint8_t> body{0,0,0,36,0,0,0,0,
        0x2f,0xe6,0x4f,0x22,0x6e,0xf3,0x94,0x0b,
        0xd1,0x03,0x41,0x0b,0,9,0x6f,0xe3,
        0x4f,0x26,0,0x0b,0x6e,0xf6,0,9,
        6,2,0x35,0x3c,0,9,0,9,uint8_t(id>>8),uint8_t(id),0,9};
    uint16_t crc=0xffff;
    for(auto byte:body){
        crc^=uint16_t(byte)<<8;
        for(unsigned bit=0;bit<8;++bit)crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    std::vector<uint8_t> wire{0x0a,0,uint8_t(crc>>8),uint8_t(crc),0,0,0,uint8_t(body.size())};
    wire.insert(wire.end(),body.begin(),body.end());wire.push_back(2);
    return wire;
}
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
