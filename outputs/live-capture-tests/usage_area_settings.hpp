#pragma once
#include "game_ranking_settings.hpp"
#include <xband/local_phone_policy.hpp>
#include <map>
#include <chrono>
#include <cstdio>
namespace diagnostic {
struct UsageDayRule {
    int mode=0; // 0 all day, 1 time window, 2 unavailable
    int start=0,end=1440; // JST minutes; end is exclusive. 24:00 allowed only as end.
    bool operator==(const UsageDayRule&)const=default;
};
struct UsageSchedule {
    bool enabled=false,includeMail=false;
    UsageDayRule weekdays{},weekend{};
    bool operator==(const UsageSchedule&)const=default;
};
inline void validateUsageSchedule(const UsageSchedule& s){
    for(const auto& r:{s.weekdays,s.weekend})
        if(r.mode<0||r.mode>2||r.start<0||r.start>=1440||r.end<0||r.end>1440||(r.mode==1&&r.start==r.end))
            throw std::runtime_error("Invalid JST time window; use all day for a 24-hour period");
}
inline std::string usageMinuteText(int minute){char buffer[8];std::snprintf(buffer,sizeof(buffer),"%02d:%02d",minute/60,minute%60);return buffer;}
inline bool usageScheduleAllows(const UsageSchedule& s,std::chrono::system_clock::time_point utc){
    validateUsageSchedule(s);if(!s.enabled)return true;
    const auto jst=utc+std::chrono::hours(9);const auto day=std::chrono::floor<std::chrono::days>(jst);
    const auto minute=std::chrono::duration_cast<std::chrono::minutes>(jst-day).count();
    const auto rule=[&](auto d)->const UsageDayRule&{const auto w=std::chrono::weekday{d}.c_encoding();return w==0||w==6?s.weekend:s.weekdays;};
    const auto& today=rule(day);const auto& previous=rule(day-std::chrono::days(1));
    if(today.mode==0)return true;
    if(today.mode==1){
        if(today.start<today.end&&minute>=today.start&&minute<today.end)return true;
        if(today.start>today.end&&minute>=today.start)return true;
    }
    // An overnight window belongs to the day it starts, including Friday->
    // Saturday and Sunday->Monday. A wholly unavailable next day does not
    // revoke the tail of an already configured previous-day window.
    return previous.mode==1&&previous.start>previous.end&&minute<previous.end;
}
struct UsagePlayTime {
    bool enabled=false;
    std::array<std::string,2> lines{
        rankingUTF8(L"\u6708\uff0d\u91d1 \u5229\u7528\u53ef\u80fd"),
        rankingUTF8(L"\u571f\uff0d\u65e5 \u5229\u7528\u53ef\u80fd")};
    bool operator==(const UsagePlayTime&)const=default;
};
inline UsagePlayTime usageScheduleDisplay(const UsageSchedule& s){
    validateUsageSchedule(s);UsagePlayTime result;result.enabled=true;
    const std::array<std::wstring,2> days{L"\u6708\uff0d\u91d1 ",L"\u571f\uff0d\u65e5 "};
    for(unsigned i=0;i<2;++i){const auto& r=i?s.weekend:s.weekdays;
        result.lines[i]=rankingUTF8(days[i])+(r.mode==0?rankingUTF8(L"\u5229\u7528\u53ef\u80fd"):
            r.mode==2?rankingUTF8(L"\u5229\u7528\u4e0d\u53ef"):usageMinuteText(r.start)+"-"+usageMinuteText(r.end));
    }
    return result;
}
inline void validateUsagePlayTime(const UsagePlayTime& value){
    for(const auto& line:value.lines){
        if(line.empty())continue;
        if(rankingEUC(line).size()>30)throw std::runtime_error("Play-time line must fit in 30 EUC-JP bytes");
    }
}
// Original usage screen callback0605CCE8 reads012E:0054 and0055 separately.
// Text only: these commands do not enforce weekday/weekend access restrictions.
inline std::vector<uint8_t> usagePlayTimeWire(const UsagePlayTime& value){
    validateUsagePlayTime(value);if(!value.enabled)return {};
    std::vector<uint8_t> out;
    for(unsigned i=0;i<2;++i){
        const auto text=value.lines[i].empty()?std::string{}:rankingEUC(value.lines[i]);
        const auto n=uint32_t(text.size()+1);
        const std::vector<uint8_t> head{0x32,0,uint8_t(0x54+i),uint8_t(n>>24),uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)};
        out.insert(out.end(),head.begin(),head.end());out.insert(out.end(),text.begin(),text.end());out.push_back(0);
    }
    return out;
}
inline std::vector<uint8_t> usageTimeDeniedNotice(bool peerRestricted=false){
    const auto text=rankingStandardEUC(rankingUTF8(peerRestricted?
        L"指名した相手は利用時間外です。時間をおいてお申し込みください。":
        L"\u73fe\u5728\u306f\u5229\u7528\u6642\u9593\u5916\u3067\u3059\u3002\u4f7f\u7528\u72b6\u6cc1\u3092\u3054\u78ba\u8a8d\u304f\u3060\u3055\u3044\u3002"));
    // Existing command22 menu notification, then existing command02 end.
    // Local server policy, not an observed historical restriction response.
    std::vector<uint8_t> out{0x22,0,1,0,0,0,120,1,44,0,0,0,uint8_t(text.size()+1)};
    out.insert(out.end(),text.begin(),text.end());out.push_back(0);out.push_back(2);return out;
}
// Original command32 updates resource012E:0052. Display text only: it does
// not change local pair admission, telephone routing or subscription rights.
inline std::vector<uint8_t> usageAreaWire(bool nationwide) {
    const auto text=rankingEUC(rankingUTF8(nationwide?L"\u5168\u56fd":L"\u540c\u4e00\u5c40\u756a\u5185"));
    const uint32_t length=uint32_t(text.size()+1);
    std::vector<uint8_t> out{0x32,0,0x52,uint8_t(length>>24),uint8_t(length>>16),uint8_t(length>>8),uint8_t(length)};
    out.insert(out.end(),text.begin(),text.end());out.push_back(0);return out;
}
// Verified VF command4C updates the separate settings item0200:0022.
// Fixed items use word0. Selectable items use word1 and both value/description
// pairs, with index0 same prefix and index1 nationwide. No preference writes.
// Historical server ordering is unknown; this is the tested local ordering.
inline std::vector<uint8_t> usageAreaSettingWire(bool nationwide,bool selectable=false) {
    std::vector<uint8_t> strings;
    auto append=[&](const std::string& text){
        const auto bytes=rankingEUC(text);strings.insert(strings.end(),bytes.begin(),bytes.end());strings.push_back(0);
    };
    append(rankingUTF8(L"\u5bfe\u6226\u30a8\u30ea\u30a2:"));
    if(selectable){
        for(const auto& text:{rankingUTF8(L"\u540c\u4e00\u5c40\u756a\u5185"),
                             rankingUTF8(L"\u5e02\u5916\u3078\u306e\u30b3\u30fc\u30eb\u306f\u3057\u307e\u305b\u3093"),
                             rankingUTF8(L"\u5168\u56fd"),
                             rankingUTF8(L"\u9577\u8ddd\u96e2\u901a\u8a71\u306b\u306a\u308b\u53ef\u80fd\u6027\u304c\u3042\u308a\u307e\u3059\u3002")})append(text);
    }else for(const auto& text:{
                         rankingUTF8(nationwide?L"\u5168\u56fd":L"\u540c\u4e00\u5c40\u756a\u5185"),
                         rankingUTF8(L"\u5bfe\u6226\u30a8\u30ea\u30a2\u306e\u8868\u793a")})append(text);
    const uint32_t size=uint32_t(strings.size()+2);
    std::vector<uint8_t> out{0x4c,uint8_t(size>>24),uint8_t(size>>16),uint8_t(size>>8),uint8_t(size),0,uint8_t(selectable)};
    out.insert(out.end(),strings.begin(),strings.end());return out;
}
class UsageAreaSettings {
    mutable std::mutex mutex;std::filesystem::path path;
    std::map<std::string,int> rows;
    std::map<std::string,UsagePlayTime> playTimes;
    std::map<std::string,UsageSchedule> schedules;
public:
    explicit UsageAreaSettings(std::filesystem::path file):path(std::move(file)) {
        if(std::filesystem::exists(path)) {
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read usage settings");
            const auto j=nlohmann::json::parse(in);
            if(j.at("schema")!=1||j.at("scope")!="registration-phone-display-only")throw std::runtime_error("Invalid usage settings schema");
            for(const auto& item:j.at("subscribers")){
                const auto phone=item.at("phone").get<std::string>();const int area=item.at("selection").get<int>();
                if(xband::phoneDigits(phone)!=phone||phone.empty()||area< -1||area>2||rows.contains(phone))throw std::runtime_error("Invalid/duplicate usage subscriber");
                rows.emplace(phone,area);
                if(item.contains("play_time")){
                    const auto& p=item.at("play_time");
                    if(!p.at("lines").is_array()||p.at("lines").size()!=2)throw std::runtime_error("Expected two play-time lines");
                    UsagePlayTime value{p.at("enabled").get<bool>(),p.at("lines").get<std::array<std::string,2>>()};
                    validateUsagePlayTime(value);playTimes.emplace(phone,std::move(value));
                }
                if(item.contains("schedule")){
                    const auto& p=item.at("schedule");UsageSchedule value;
                    if(p.at("timezone")!="Asia/Tokyo")throw std::runtime_error("Schedule timezone must be Asia/Tokyo");
                    value.enabled=p.at("enabled").get<bool>();value.includeMail=p.at("include_mail").get<bool>();
                    auto readRule=[](const auto& j){return UsageDayRule{j.at("mode").template get<int>(),j.at("start").template get<int>(),j.at("end").template get<int>()};};
                    value.weekdays=readRule(p.at("weekdays"));value.weekend=readRule(p.at("weekend"));
                    validateUsageSchedule(value);schedules.emplace(phone,value);
                }
            }
        }
    }
    // -1 no override;0 fixed same prefix;1 fixed nationwide;2 ROM selectable.
    int selection(const std::string& phone)const {std::lock_guard lock(mutex);const auto it=rows.find(xband::phoneDigits(phone));return it==rows.end()?-1:it->second;}
    std::map<std::string,int> snapshot()const {std::lock_guard lock(mutex);return rows;}
    UsagePlayTime playTime(const std::string& phone)const {std::lock_guard lock(mutex);const auto it=playTimes.find(xband::phoneDigits(phone));return it==playTimes.end()?UsagePlayTime{}:it->second;}
    UsageSchedule schedule(const std::string& phone)const {std::lock_guard lock(mutex);const auto it=schedules.find(xband::phoneDigits(phone));return it==schedules.end()?UsageSchedule{}:it->second;}
    bool accessAllowed(const std::string& phone,bool mail,std::chrono::system_clock::time_point utc=std::chrono::system_clock::now())const {
        const auto s=schedule(phone);return mail&&!s.includeMail?true:usageScheduleAllows(s,utc);
    }
    void observe(const std::string& phone){const auto key=xband::phoneDigits(phone);if(key.empty())return;std::lock_guard lock(mutex);rows.try_emplace(key,-1);}
    void save(const std::string& phone,int selected,std::optional<UsagePlayTime> play=std::nullopt,std::optional<UsageSchedule> schedule=std::nullopt) {
        if(selected< -1||selected>2)throw std::runtime_error("Invalid area selection");
        if(play)validateUsagePlayTime(*play);
        if(schedule)validateUsageSchedule(*schedule);
        const auto key=xband::phoneDigits(phone);if(key.empty())throw std::runtime_error("Enter the registration phone number (digits/hyphens only)");
        std::lock_guard lock(mutex);
        auto staged=rows;auto stagedPlay=playTimes;auto stagedSchedule=schedules;
        staged[key]=selected;if(play)stagedPlay[key]=*play;if(schedule)stagedSchedule[key]=*schedule;
        auto subscribers=nlohmann::json::array();
        for(const auto& [number,area]:staged){
            nlohmann::json item{{"phone",number},{"selection",area}};
            if(const auto it=stagedPlay.find(number);it!=stagedPlay.end())
                item["play_time"]={{"enabled",it->second.enabled},{"lines",it->second.lines}};
            if(const auto it=stagedSchedule.find(number);it!=stagedSchedule.end()){
                const auto& s=it->second;auto rule=[](const UsageDayRule& r){return nlohmann::json{{"mode",r.mode},{"start",r.start},{"end",r.end}};};
                item["schedule"]={{"enabled",s.enabled},{"include_mail",s.includeMail},{"timezone","Asia/Tokyo"},{"weekdays",rule(s.weekdays)},{"weekend",rule(s.weekend)}};
            }
            subscribers.push_back(std::move(item));
        }
        nlohmann::json j{{"schema",1},{"scope","registration-phone-display-only"},{"subscribers",subscribers}};
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".tmp";
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<j.dump(2);out.flush();if(!out)throw std::runtime_error("Cannot write usage settings");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Cannot back up usage settings");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit usage settings");
        rows=std::move(staged);playTimes=std::move(stagedPlay);schedules=std::move(stagedSchedule);
    }
    std::vector<uint8_t> wire(const std::string& phone,std::optional<uint32_t> game=std::nullopt,
                             std::optional<uint8_t> reportedArea=std::nullopt)const {
        // One settings snapshot keeps both lines and the area consistent with
        // an atomic Save, even when the UI and service thread run concurrently.
        int selected=-1;UsagePlayTime play;UsageSchedule schedule;
        {std::lock_guard lock(mutex);const auto key=xband::phoneDigits(phone);
            if(const auto it=rows.find(key);it!=rows.end())selected=it->second;
            if(const auto it=playTimes.find(key);it!=playTimes.end())play=it->second;
            if(const auto it=schedules.find(key);it!=schedules.end())schedule=it->second;}
        if(schedule.enabled)play=usageScheduleDisplay(schedule);
        std::vector<uint8_t> out;
        if(selected==0||selected==1)out=usageAreaWire(selected==1);
        // Unknown/missing values must not invent a selected user's preference.
        // The two-choice resource does not replace variable49 or other users.
        else if(selected==2&&reportedArea&&*reportedArea<=1)out=usageAreaWire(*reportedArea==1);
        // Shared XOS command4C updates the application's connection setting.
        if(game&&selected>=0){const auto setting=usageAreaSettingWire(selected==1,selected==2);out.insert(out.end(),setting.begin(),setting.end());}
        const auto time=usagePlayTimeWire(play);out.insert(out.end(),time.begin(),time.end());
        return out;
    }
};
inline std::shared_ptr<UsageAreaSettings> activeUsageArea;
}
