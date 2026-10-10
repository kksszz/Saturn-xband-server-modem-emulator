#pragma once
#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <functional>
namespace diagnostic {
inline std::wstring rankingWide(const std::string &s){
    if(s.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    if(!n)throw std::runtime_error("Invalid UTF-8 text");
    std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),w.data(),n);return w;
}
inline std::string rankingUTF8(const std::wstring &w){
    if(w.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);
    if(!n)throw std::runtime_error("Invalid Unicode text");
    std::string s(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w.data(),int(w.size()),s.data(),n,nullptr,nullptr);return s;
}
// Saturn XBAND strings use EUC-JP, not UTF-8 or Shift-JIS. Convert only
// standard JIS characters; reject unsupported/best-fit characters explicitly.
inline std::string rankingStandardEUC(const std::string &utf8){
    auto w=rankingWide(utf8);if(w.empty())throw std::runtime_error("Text must not be empty");
    if(w.find(L'\0')!=w.npos)throw std::runtime_error("Embedded NUL is not allowed");
    BOOL substituted=FALSE;
    int n=WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,w.data(),int(w.size()),nullptr,0,nullptr,&substituted);
    std::string sjis(n,0);
    if(!n||!WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,w.data(),int(w.size()),sjis.data(),n,nullptr,&substituted)||substituted)
        throw std::runtime_error("Character is not available in the game font");
    std::string euc;
    for(size_t i=0;i<sjis.size();++i){unsigned a=uint8_t(sjis[i]);
        if(a<0x80){if(a<0x20||a==0x7f)throw std::runtime_error("Control character is not allowed");euc+=char(a);}
        else if(a>=0xa1&&a<=0xdf){euc+=char(0x8e);euc+=char(a);}
        else {
            if(++i>=sjis.size())throw std::runtime_error("Incomplete Japanese character");
            unsigned b=uint8_t(sjis[i]);
            unsigned row=(a<=0x9f?a-0x81:a-0xc1)*2+0x21,col;
            if(b>=0x9f){++row;col=b-0x7e;}else col=b-(b>0x7f?0x20:0x1f);
            if(row<0x21||row>0x7e||col<0x21||col>0x7e)throw std::runtime_error("Nonstandard JIS character is not supported");
            euc+=char(row|0x80);euc+=char(col|0x80);
        }
    }
    if(euc.size()>64)throw std::runtime_error("Each field must fit in 64 encoded bytes");
    return euc;
}
// Original VF SH-2 decoder maps single-byte 85 to font 1102 glyph 0105.
// Settings/UI stay UTF-8; only the outbound XBAND representation is extended.
inline std::string rankingEUC(const std::string &utf8){
    auto w=rankingWide(utf8);if(w.empty())throw std::runtime_error("Text must not be empty");
    std::string out;size_t begin=0;
    for(size_t i=0;i<=w.size();++i){
        if(i<w.size()&&w[i]!=L'\u2122')continue;
        if(i>begin)out+=rankingStandardEUC(rankingUTF8(w.substr(begin,i-begin)));
        if(i<w.size())out+=char(0x85);
        begin=i+1;
    }
    if(out.size()>64)throw std::runtime_error("Each field must fit in 64 encoded bytes");
    return out;
}
struct GameRankingRow {uint32_t gameID;uint8_t slot;std::array<std::string,5> fields;int winPoints=-1;int losePoints=0;};
struct GameMatchAward {std::vector<uint8_t> wire;int winPoints=-1;int losePoints=0;bool known=false;};
// Server policy for newly registered games; not historical award values.
inline constexpr int newGameDefaultWinPoints=1,newGameDefaultLosePoints=0;
// Verified game IDs are application defaults, not private runtime data.
// Keep this catalog in source control and releases; saved settings override it.
inline std::vector<GameRankingRow> rankingDefaults(){return {
    {0x00010003,1,{rankingUTF8(L"Virtua Fighter\u2122 Remix"),rankingUTF8(L"\u4e94\u6bb5"),"2156",rankingUTF8(L"\u516d\u6bb5"),"2844"}},
    {0x00018003,2,{"PUYO PUYO SUN",rankingUTF8(L"17\u7d1a"),"13",rankingUTF8(L"16\u7d1a"),"7"}},
    {0x00018002,3,{"ID 0x00018002","LEVEL 1","100","LEVEL 2","50"}},
    {0x00010007,4,{"DECATHLETE XBAND","LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints},
    {0x00010006,5,{"DAYTONA USA CIRCUIT EDITION","LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints},
    {0x00010004,6,{"ID 0x00010004","LEVEL 1","100","LEVEL 2","50"}},
    {0x00018001,7,{"SATURN BOMBERMAN","LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints},
    {0x00010005,8,{"VIRTUAL-ON","LEVEL 1","100","LEVEL 2","50"}},
    {0x00010008,9,{"SEGA WORLD WIDE SOCCER 98","LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints},
    {0x00018004,10,{"SHADOWS OF THE TUSK","LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints}
};}
inline void validateRanking(const GameRankingRow &row){
    if(row.winPoints < -1 || row.winPoints > 999)throw std::runtime_error("Match award must be unset (-1) or 0..999");
    if(row.losePoints < 0 || row.losePoints > 999)throw std::runtime_error("Losing award must be 0..999");
    size_t bytes=6;for(const auto &s:row.fields)bytes+=rankingEUC(s).size()+1;
    if(bytes+4>999)throw std::runtime_error("Ranking record too large");
    for(unsigned i:{2u,4u}){
        const auto &s=row.fields[i];
        if(s.empty()||s.size()>9||s.find_first_not_of("0123456789")!=s.npos)
            throw std::runtime_error("Points must be 0..999999999, decimal digits only");
    }
}
class GameRankingSettings {
    mutable std::mutex mutex;
    std::vector<GameRankingRow> rows=rankingDefaults();std::filesystem::path path;
    static nlohmann::json encode(const std::vector<GameRankingRow> &values){
        nlohmann::json games=nlohmann::json::array();
        for(const auto &r:values)games.push_back({{"game_id",r.gameID},{"fields",r.fields},{"win_points",r.winPoints},{"lose_points",r.losePoints}});
        return {{"schema",1},{"policy","server-win-loss-awards"},{"games",games}};
    }
public:
    explicit GameRankingSettings(std::filesystem::path file):path(std::move(file)){
        if(std::filesystem::exists(path)){
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read ranking settings");
            auto j=nlohmann::json::parse(in);
            if(j.at("schema")!=1||!j.at("games").is_array()||j.at("games").size()<8||j.at("games").size()>63)throw std::runtime_error("Invalid ranking settings schema");
            std::vector<uint32_t> seen;
            for(const auto &item:j.at("games")){
                const auto& value=item.at("game_id");
                if(!value.is_number_unsigned()||value.get<uint64_t>()==0||value.get<uint64_t>()>=0xffffffffULL)throw std::runtime_error("Invalid game ID");
                uint32_t id=value.get<uint32_t>();auto it=std::find_if(rows.begin(),rows.end(),[&](const auto&r){return r.gameID==id;});
                if(std::find(seen.begin(),seen.end(),id)!=seen.end())throw std::runtime_error("Duplicate game ID in ranking settings");
                if(it==rows.end()){if(rows.size()>=63)throw std::runtime_error("Maximum63 games");rows.push_back({id,uint8_t(rows.size()+1),{"","LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints});it=rows.end()-1;}
                it->fields=item.at("fields").get<std::array<std::string,5>>();
                auto award=[&](const char* key,int fallback,int minimum){
                    if(!item.contains(key))return fallback;const auto& v=item.at(key);
                    if(!v.is_number_integer()||v<minimum||v>999)throw std::runtime_error("Invalid saved award range/type");
                    return v.get<int>();
                };
                it->winPoints=award("win_points",it->winPoints,-1);it->losePoints=award("lose_points",0,0);validateRanking(*it);seen.push_back(id);
                // Promote only the legacy unverified DecAthlete placeholder.
                // Keep its slot/ID, user totals and explicitly enabled awards.
                if(id==0x00010007&&it->fields[0]=="ID 0x00010007"){
                    it->fields[0]="DECATHLETE XBAND";
                    if(it->winPoints==-1)it->winPoints=newGameDefaultWinPoints;
                }
            }
            for(const auto& r:rankingDefaults())if(r.slot<=8&&std::find(seen.begin(),seen.end(),r.gameID)==seen.end())throw std::runtime_error("Missing original game setting");
        }
    }
    std::vector<GameRankingRow> snapshot()const{std::lock_guard lock(mutex);return rows;}
private:
    void commit(const std::vector<GameRankingRow>& staged){
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());auto tmp=path;tmp+=L".tmp";
        {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out<<encode(staged).dump(2);out.flush();if(!out)throw std::runtime_error("Cannot write ranking settings; previous values preserved");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Cannot back up ranking settings");}
        if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit ranking settings");
        rows=staged;
    }
public:
    void add(uint32_t id,const std::string& title){
        std::lock_guard lock(mutex);
        if(id==0||id==0xffffffffu)throw std::runtime_error("Game ID must be 1..FFFFFFFE");
        if(std::any_of(rows.begin(),rows.end(),[&](const auto& r){return r.gameID==id;}))throw std::runtime_error("Game ID already registered");
        if(rows.size()>=63)throw std::runtime_error("Maximum63 games (ROM resource slots)");
        auto staged=rows;GameRankingRow row{id,uint8_t(rows.size()+1),{title,"LEVEL 1","0","LEVEL 2","200"},newGameDefaultWinPoints,newGameDefaultLosePoints};
        validateRanking(row);staged.push_back(row);commit(staged);
    }
    void update(uint32_t id,const std::array<std::string,5>&fields,std::optional<int> winPoints={},std::optional<int> losePoints={}){
        std::lock_guard lock(mutex);auto staged=rows;
        auto it=std::find_if(staged.begin(),staged.end(),[&](const auto&r){return r.gameID==id;});
        if(it==staged.end())throw std::runtime_error("Unknown game ID");
        it->fields=fields;if(winPoints)it->winPoints=*winPoints;if(losePoints)it->losePoints=*losePoints;validateRanking(*it);
        commit(staged);
    }
    std::vector<uint8_t> introTitleWire(std::optional<uint32_t> id)const{
        // Shared XOS callback 0605CEDE reads writable string008B,
        // independently of ranking0116 and award strings00B4/00B5.
        if(!id)return {};
        auto values=snapshot();auto it=std::find_if(values.begin(),values.end(),[&](const auto&r){return r.gameID==*id;});
        if(it==values.end())return {};
        // Only the opponent-introduction resource uses this historical short
        // label. Ranking fields, saved settings and custom names stay unchanged.
        const auto label=(*id==0x00010003&&it->fields[0]==rankingUTF8(L"Virtua Fighter\u2122 Remix"))
            ?rankingUTF8(L"VF\u2122 Remix"):it->fields[0];
        const auto text=rankingEUC(label);const auto n=text.size()+1;
        std::vector<uint8_t> out{0x32,0,0x8b,uint8_t(n>>24),uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)};
        out.insert(out.end(),text.begin(),text.end());out.push_back(0);return out;
    }
    GameMatchAward matchAward(std::optional<uint32_t> id)const{
        if(!id)return {};
        auto values=snapshot();auto it=std::find_if(values.begin(),values.end(),[&](const auto&r){return r.gameID==*id;});
        if(it==values.end())return {};
        if(it->winPoints==-1)return {{},-1,it->losePoints,true};
        // These resources are local/peer winning worth, NOT win/loss slots.
        // Losing awards are applied by the correlated server result ledger
        // and synchronized in command25; do not reinterpret the ROM strings.
        std::string text(1,char(9));
        text+=it->winPoints>0?std::to_string(it->winPoints):rankingEUC(rankingUTF8(L"\u30ce\u30fc"));
        text+='\n';text+=rankingEUC(rankingUTF8(L"\u30dd\u30a4\u30f3\u30c8"));text.push_back(0);
        const auto n=text.size();std::vector<uint8_t> out;
        // Both sides have the same custom worth, so peer role reversal cannot
        // change the rule. Keep local/peer worth separate from title008B.
        for(uint8_t resource:{uint8_t(0xb4),uint8_t(0xb5)}){
            out.insert(out.end(),{0x32,0,resource,uint8_t(n>>24),uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)});
            out.insert(out.end(),text.begin(),text.end());
        }
        return {std::move(out),it->winPoints,it->losePoints,true};
    }
    std::vector<uint8_t> matchAwardWire(std::optional<uint32_t> id)const{
        return matchAward(id).wire;
    }
    std::vector<uint8_t> wire(std::optional<uint32_t> id,std::function<std::optional<uint32_t>(uint8_t)> points={},
                             std::function<std::array<std::string,5>(uint8_t,const std::array<std::string,5>&)> profileFields={})const{
        if(!id)return {};auto values=snapshot();auto it=std::find_if(values.begin(),values.end(),[&](const auto&r){return r.gameID==*id;});
        if(it==values.end())return {};
        std::vector<uint8_t> out;
        for(uint8_t user:{uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(3)}){
            if(!profileFields&&user!=0&&user!=3)continue; // Legacy display fixtures remain unchanged.
            std::vector<uint8_t> body{uint8_t(*id>>24),uint8_t(*id>>16),uint8_t(*id>>8),uint8_t(*id),user,0};
            auto fields=it->fields;
            if(profileFields)fields=profileFields(user,fields);
            if(points)if(auto value=points(user))fields[2]=std::to_string(*value);
            for(const auto &s:fields){auto euc=rankingEUC(s);body.insert(body.end(),euc.begin(),euc.end());body.push_back(0);}
            const uint16_t resourceID=uint16_t(user)*64+it->slot;
            out.insert(out.end(),{0x25,uint8_t(resourceID>>8),uint8_t(resourceID),uint8_t(body.size()>>8),uint8_t(body.size())});
            out.insert(out.end(),body.begin(),body.end());out.insert(out.end(),4,0);
        }
        return out;
    }
};
inline std::shared_ptr<GameRankingSettings> activeGameRankings;
}
