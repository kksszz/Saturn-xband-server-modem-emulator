#pragma once
#include "game_ranking_settings.hpp"
#include <map>
namespace diagnostic {
// Configurable local policy: 30 ranks, initially 200-point spacing.
// This is not claimed to be Sega's historical rank table.
class GameLevelSettings {
public:
    using Thresholds=std::array<uint32_t,30>;
    struct Display {unsigned rank,next;uint32_t required;};
    struct Appearance {std::string unit="LEVEL";bool descending=false;};
    static Thresholds uniform(uint32_t step){
        if(!step||step>999999999/29)throw std::runtime_error("Rank spacing must be 1..34482758");
        Thresholds values{};for(unsigned i=0;i<30;++i)values[i]=i*step;return values;
    }
private:
    using J=nlohmann::json;
    std::filesystem::path path;mutable std::mutex mutex;std::map<uint32_t,Thresholds> rows;
    std::map<uint32_t,Appearance> formats;
    static void validateAppearance(const Appearance& a){if(a.unit!="LEVEL"&&a.unit!=rankingUTF8(L"段")&&a.unit!=rankingUTF8(L"級"))throw std::runtime_error("Select LEVEL, dan or kyu");}
    static void validate(const Thresholds& t){
        if(t[0]!=0)throw std::runtime_error("Rank1 starts at0");
        for(unsigned i=1;i<30;++i)if(t[i]<=t[i-1]||t[i]>999999999)throw std::runtime_error("Thresholds must increase strictly (0..999999999)");
    }
    void commit(const std::map<uint32_t,Thresholds>& staged,const std::map<uint32_t,Appearance>& appearance){
        J games=J::array();for(const auto& [id,t]:staged)games.push_back({{"game_id",id},{"thresholds",t},{"unit",appearance.at(id).unit},{"descending",appearance.at(id).descending}});
        auto pending=path;pending+=L".pending";if(std::filesystem::exists(pending))throw std::runtime_error("Incomplete level settings; preserve and recover");
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        {std::ofstream out(pending,std::ios::binary);out<<J{{"schema",1},{"policy","local-30-ranks-configurable"},{"games",games}}.dump(2);out.flush();if(!out)throw std::runtime_error("Level settings write failed");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Level settings backup failed");}
        if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Level settings commit failed");
        rows=staged;formats=appearance;
    }
public:
    explicit GameLevelSettings(std::filesystem::path file):path(std::move(file)){
        for(const auto& r:rankingDefaults()){rows.emplace(r.gameID,uniform(200));formats.emplace(r.gameID,Appearance{});}
        formats.at(0x10003)={rankingUTF8(L"段"),false};formats.at(0x18003)={rankingUTF8(L"級"),true};
        auto pending=path;pending+=L".pending";if(std::filesystem::exists(pending))throw std::runtime_error("Incomplete level settings; preserve and recover");
        if(!std::filesystem::exists(path)){commit(rows,formats);return;}
        if(std::filesystem::file_size(path)>65536)throw std::runtime_error("Oversized level settings");
        std::ifstream in(path);J j;in>>j;if(j.at("schema")!=1||!j.at("games").is_array()||j.at("games").size()<8||j.at("games").size()>63)throw std::runtime_error("Invalid level settings");
        std::vector<uint32_t> seen;
        for(const auto& g:j.at("games")){const auto& value=g.at("game_id");
            if(!value.is_number_unsigned()||value.get<uint64_t>()==0||value.get<uint64_t>()>=0xffffffffULL)throw std::runtime_error("Invalid level game ID");
            const auto id=value.get<uint32_t>();
            if(std::find(seen.begin(),seen.end(),id)!=seen.end())throw std::runtime_error("Duplicate level game");
            rows.try_emplace(id,uniform(200));formats.try_emplace(id,Appearance{});
            if(!g.at("thresholds").is_array()||g.at("thresholds").size()!=30)throw std::runtime_error("Exactly30 thresholds required");
            for(const auto& v:g.at("thresholds"))if(!v.is_number_unsigned()||v.get<uint64_t>()>999999999)throw std::runtime_error("Invalid level threshold");
            const auto t=g.at("thresholds").get<Thresholds>();validate(t);rows.at(id)=t;seen.push_back(id);
            if(g.contains("unit")){Appearance a{g.at("unit").get<std::string>(),g.at("descending").get<bool>()};validateAppearance(a);formats.at(id)=a;}
        }
        for(const auto& r:rankingDefaults())if(r.slot<=8&&std::find(seen.begin(),seen.end(),r.gameID)==seen.end())throw std::runtime_error("Missing original level setting");
    }
    Thresholds thresholds(uint32_t game)const{std::lock_guard lock(mutex);auto it=rows.find(game);return it==rows.end()?uniform(200):it->second;}
    void save(uint32_t game,const Thresholds& values){if(!game||game==0xffffffffu)throw std::runtime_error("Invalid level game ID");validate(values);std::lock_guard lock(mutex);auto staged=rows;auto appearance=formats;staged[game]=values;appearance.try_emplace(game,Appearance{});if(staged.size()>63)throw std::runtime_error("Maximum63 level games");commit(staged,appearance);}
    Appearance appearance(uint32_t game)const{std::lock_guard lock(mutex);auto it=formats.find(game);return it==formats.end()?Appearance{}:it->second;}
    void saveAppearance(uint32_t game,Appearance a){if(!game||game==0xffffffffu)throw std::runtime_error("Invalid level game ID");validateAppearance(a);std::lock_guard lock(mutex);auto staged=formats;auto thresholds=rows;staged[game]=a;thresholds.try_emplace(game,uniform(200));if(staged.size()>63)throw std::runtime_error("Maximum63 level games");commit(thresholds,staged);}
    std::string label(uint32_t game,unsigned rank)const{
        if(rank<1||rank>30)throw std::runtime_error("Rank outside1..30");const auto a=appearance(game);const auto n=a.descending?31-rank:rank;
        if(a.unit=="LEVEL")return "LEVEL "+std::to_string(n);
        if(a.unit==rankingUTF8(L"段")){
            const wchar_t* digits[]={L"",L"一",L"二",L"三",L"四",L"五",L"六",L"七",L"八",L"九"};
            std::wstring s;if(n>=10){if(n>=20)s+=digits[n/10];s+=L"十";}s+=digits[n%10];return rankingUTF8(s)+a.unit;
        }
        return std::to_string(n)+a.unit;
    }
    Display display(uint32_t game,uint32_t points)const{
        const auto t=thresholds(game);const unsigned index=unsigned(std::upper_bound(t.begin(),t.end(),points)-t.begin()-1);
        return {index+1,index==29?0:index+2,index==29?0:t[index+1]-points};
    }
};
inline std::shared_ptr<GameLevelSettings> activeGameLevels;
}
