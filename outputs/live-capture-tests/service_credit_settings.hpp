#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace diagnostic {
// Server-local policy. Mail unit is PER SENT LETTER, not per connection.
// enabling mail consumption is explicit, not a migration side effect.
struct ServiceCreditValues {
    bool configured=false;
    unsigned mail=1,match=3;
    bool mailEnabled=false;
    bool matchEnabled=false;
    int resetScope=1,includeMail=1; // Both interrupted participants1, plus actual outgoing letters only.
    static constexpr unsigned reset=1; // User-selected common total, not3+1.
    bool operator==(const ServiceCreditValues&)const=default;
};
class ServiceCreditSettings {
    std::filesystem::path path;
    mutable std::mutex mutex;
    ServiceCreditValues values;
public:
    explicit ServiceCreditSettings(std::filesystem::path file):path(std::move(file)){
        if(!std::filesystem::exists(path))return;
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read service credit settings");
        const auto j=nlohmann::json::parse(in);
        const bool legacy=j.at("schema")==1&&j.at("activation")=="draft-not-active";
        const bool mailOnly=j.at("schema")==2&&j.at("activation")=="explicit-mail-policy";
        const bool matchPolicy=j.at("schema")==3&&j.at("activation")=="explicit-service-policy";
        const bool sendPolicy=j.at("schema")==4&&j.at("activation")=="explicit-per-letter-policy";
        if(j.at("scope")!="server-global"||(!legacy&&!mailOnly&&!matchPolicy&&!sendPolicy))
            throw std::runtime_error("Invalid service credit settings schema");
        if(sendPolicy&&j.at("mail_basis")!="accepted-outgoing-letter")throw std::runtime_error("Invalid mail billing basis");
        const auto& amounts=j.at("requested_units");
        for(const char* key:{"mail","match"}){
            const auto& n=amounts.at(key);
            if(!n.is_number_integer()||n.get<int64_t>()<0||n.get<int64_t>()>32767)
                throw std::runtime_error("Requested units must be integer0..32767 (local verified envelope)");
        }
        values={true,amounts.at("mail").get<unsigned>(),amounts.at("match").get<unsigned>(),legacy?false:j.at("mail_enabled").get<bool>()};
        if(!legacy&&amounts.at("reset")!=1)throw std::runtime_error("Reset total must remain1");
        if(matchPolicy||sendPolicy){
            if(!j.at("reset_scope").is_number_integer()||!j.at("include_mail").is_number_integer())throw std::runtime_error("Integer policy fields required");
            values.matchEnabled=j.at("match_enabled").get<bool>();values.resetScope=j.at("reset_scope").get<int>();values.includeMail=j.at("include_mail").get<int>();
            if(values.resetScope!=1||values.includeMail!=1)throw std::runtime_error("Both-side reset1 and additive outgoing mail policy required");}
    }
    ServiceCreditValues snapshot()const{std::lock_guard lock(mutex);return values;}
    void save(unsigned mail,unsigned match,bool enableMail=false,bool enableMatch=false){
        if(mail>32767||match>32767)throw std::runtime_error("Requested units must be0..32767");
        std::lock_guard lock(mutex);
        const nlohmann::json j{{"schema",4},{"scope","server-global"},{"activation","explicit-per-letter-policy"},{"mail_basis","accepted-outgoing-letter"},
            {"mail_enabled",enableMail},{"match_enabled",enableMatch},{"reset_scope",1},{"include_mail",1},
            {"requested_units",{{"mail",mail},{"match",match},{"reset",1}}}};
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        auto temp=path;temp+=L".tmp";
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<j.dump(2);out.flush();if(!out)throw std::runtime_error("Cannot write service credit settings");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Cannot back up service credit settings");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit service credit settings");
        values={true,mail,match,enableMail,enableMatch,1,1};
    }
};
}
