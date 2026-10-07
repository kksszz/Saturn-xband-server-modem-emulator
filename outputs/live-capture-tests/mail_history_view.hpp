#pragma once
#include "game_ranking_settings.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <sstream>
#include <iomanip>
namespace diagnostic {
inline std::wstring mailHistoryTime(uint64_t ms){
    // Explicit JST, independent of the server computer's timezone.
    if(ms>3000000000000ull)return L"Unknown";
    const uint64_t ticks=(ms+9ull*60*60*1000)*10000+116444736000000000ull;
    FILETIME f{DWORD(ticks),DWORD(ticks>>32)};SYSTEMTIME t{};
    if(!FileTimeToSystemTime(&f,&t))return L"Unknown";
    std::wostringstream s;s<<std::setfill(L'0')<<t.wYear<<L'-'<<std::setw(2)<<t.wMonth<<L'-'
        <<std::setw(2)<<t.wDay<<L' '<<std::setw(2)<<t.wHour<<L':'<<std::setw(2)<<t.wMinute<<L':'<<std::setw(2)<<t.wSecond;
    return s.str();
}
inline std::wstring mailHistoryState(const nlohmann::json& row){
    const auto state=row.value("queue_state",std::string{});
    if(state=="pending")return L"\u9001\u4fe1\u5f85\u3061";
    if(state=="prepared_unconfirmed")return L"\u5fdc\u7b54\u3078\u683c\u7d0d\uff08\u53d7\u4fe1\u672a\u78ba\u8a8d\uff09";
    return rankingWide(state);
}
inline std::array<std::wstring,8> mailHistoryCells(const nlohmann::json& row){
    const auto name=row.value("source_name",nlohmann::json());
    return {std::to_wstring(row.at("submission_id").get<uint64_t>())+L":"+std::to_wstring(row.at("record_index").get<size_t>()+1),
        mailHistoryTime(row.at("accepted_unix_ms").get<uint64_t>()),
        name.is_string()?rankingWide(name.get<std::string>()):L"\u672a\u7279\u5b9a",
        rankingWide(row.at("phone").get<std::string>()),
        std::to_wstring(row.at("player").get<unsigned>()+1),
        rankingWide(row.at("recipient").get<std::string>()),
        rankingWide(row.at("subject").get<std::string>()),mailHistoryState(row)};
}
inline std::wstring mailHistoryDetail(const nlohmann::json& row,bool raw){
    if(raw){auto text=rankingWide(row.dump(2));std::wstring lines;
        for(auto c:text){if(c==L'\n')lines+=L'\r';lines+=c;}return lines;}
    const auto c=mailHistoryCells(row);
    std::wstring capacity;
    if(row.contains("recipient_mailbox")){
        const auto& inbox=row.at("recipient_mailbox");
        capacity=L"\r\n\u5b9b\u5148\u306e\u76f4\u8fd1\u30a2\u30af\u30bb\u30b9: \u30e6\u30fc\u30b6\u30fc\u67a0 "+std::to_wstring(inbox.at("profile").get<unsigned>()+1)+
            L" / \u53d7\u4fe1\u7bb1\u5bb9\u91cf "+std::to_wstring(inbox.at("capacity").get<size_t>())+
            L" / ROM\u7533\u544a "+std::to_wstring(inbox.at("reported").get<size_t>())+
            L" / \u5fdc\u7b54\u683c\u7d0d "+std::to_wstring(inbox.at("prepared").get<size_t>())+
            L" / \u30b5\u30fc\u30d0\u30fc\u6b8b\u4ef6 "+std::to_wstring(inbox.at("remaining").get<size_t>());
    }
    return L"ID: "+c[0]+L"    \u53d7\u4ed8\u65e5\u6642 (JST): "+c[1]+L"\r\n\u9001\u4fe1\u8005: "+c[2]+L"    \u96fb\u8a71: "+c[3]+L"    \u30e6\u30fc\u30b6\u30fc\u67a0: "+c[4]+
        L"\r\n\u5b9b\u5148: "+c[5]+L"    \u4ef6\u540d: "+c[6]+L"\r\n\u72b6\u614b: "+c[7]+capacity+
        L"\r\n\r\n\u672c\u6587\uff08ROM\u5f62\u5f0f\u30fb\u672a\u89e3\u8aad\u306e16\u9032\u30c7\u30fc\u30bf\uff09:\r\n"+
        rankingWide(row.at("encoded_body_hex").get<std::string>());
}
}
