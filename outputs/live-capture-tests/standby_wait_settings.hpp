#pragma once
#include "game_ranking_settings.hpp"
namespace diagnostic {
inline constexpr uint32_t standbyWaitTicksPerMinute=3600;
inline constexpr unsigned standbyWaitMaxMinutes=60;
struct StandbyWaitValues {
    // Original preference indices: 0 short, 1 normal, 2 long.
    std::array<unsigned,3> minutes{5,10,12};
    bool operator==(const StandbyWaitValues&)const=default;
};
inline void validateStandbyWait(const StandbyWaitValues& value){
    for(auto n:value.minutes)if(n<1||n>standbyWaitMaxMinutes)
        throw std::runtime_error("Wait time must be 1..60 minutes");
    if(value.minutes[0]>value.minutes[1]||value.minutes[1]>value.minutes[2])
        throw std::runtime_error("Wait times must be short <= normal <= long");
}
inline uint32_t standbyWaitTicks(uint8_t preference,const StandbyWaitValues& value={}){
    validateStandbyWait(value);
    if(preference>2)throw std::runtime_error("Invalid standby wait preference");
    return uint32_t(value.minutes[preference])*standbyWaitTicksPerMinute;
}
// One server-wide policy, no subscriber, user/profile or game lookup.
class StandbyWaitSettings {
    mutable std::mutex mutex;std::filesystem::path path;StandbyWaitValues values;
public:
    explicit StandbyWaitSettings(std::filesystem::path file):path(std::move(file)){
        if(!std::filesystem::exists(path))return;
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read standby wait settings");
        const auto j=nlohmann::json::parse(in);
        if(j.at("schema")!=1||j.at("scope")!="server-global")throw std::runtime_error("Invalid standby wait settings schema");
        const auto& m=j.at("minutes");
        for(const char* key:{"short","normal","long"}){
            const auto& n=m.at(key);
            if(!n.is_number_integer()||n.get<uint64_t>()<1||n.get<uint64_t>()>standbyWaitMaxMinutes)
                throw std::runtime_error("Wait time must be an integer in 1..60 minutes");
        }
        values.minutes={m.at("short").get<unsigned>(),m.at("normal").get<unsigned>(),m.at("long").get<unsigned>()};
        validateStandbyWait(values);
    }
    StandbyWaitValues snapshot()const{std::lock_guard lock(mutex);return values;}
    uint32_t ticks(uint8_t preference)const{return standbyWaitTicks(preference,snapshot());}
    void save(const StandbyWaitValues& next){
        validateStandbyWait(next);std::lock_guard lock(mutex);
        const nlohmann::json j{{"schema",1},{"scope","server-global"},
            {"minutes",{{"short",next.minutes[0]},{"normal",next.minutes[1]},{"long",next.minutes[2]}}}};
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        auto temp=path;temp+=L".tmp";
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<j.dump(2);out.flush();if(!out)throw std::runtime_error("Cannot write standby wait settings");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Cannot back up standby wait settings");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit standby wait settings");
        values=next;
    }
};
inline std::shared_ptr<StandbyWaitSettings> activeStandbyWait;
}
