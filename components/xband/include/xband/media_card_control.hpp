#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <stdexcept>

namespace xband {
enum class CardMatchAdmission { allowed, missing, empty, unreadable, unavailable };
// Local management telemetry only. Never a service/card debit request.
class MediaCardControl {
    using J=nlohmann::json;
    J report=J::object();
    std::optional<J> command;
    uint64_t seen=0,sequence=0;
    const std::string epoch=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
public:
    static constexpr uint64_t staleAfterMs=2000;
    static void validateReport(const J& r){
        if(r.at("version")!=1||!r.at("configured").is_boolean()||!r.at("inserted").is_boolean()||
           !r.at("save_failed").is_boolean()||!r.at("command_token").is_string())
            throw std::invalid_argument("Invalid media card telemetry");
        const auto& units=r.at("units");
        if(r.contains("read_fault")&&(!r.at("read_fault").is_boolean()||
           (r.at("read_fault").get<bool>()&&!units.is_null())))
            throw std::invalid_argument("Invalid media card read fault");
        if(!units.is_null()&&(!units.is_number_integer()||units.get<int64_t>()<0||units.get<int64_t>()>32767))
            throw std::invalid_argument("Invalid media card units");
        if(r.at("command_token").get<std::string>().size()>96||
           (!r.at("configured").get<bool>()&&(r.at("inserted").get<bool>()||!units.is_null()))||
           (r.at("save_failed").get<bool>()&&!units.is_null()))
            throw std::invalid_argument("Invalid media card state");
    }
    static std::string validateCommand(const J& c){
        if(c.contains("image_path")&&(!c.at("image_path").is_string()||c.at("image_path").get<std::string>().empty()||
            c.at("image_path").get<std::string>().size()>4096||c.at("image_path").get<std::string>().find('\0')!=std::string::npos||c.at("inserted")!=false))
            throw std::invalid_argument("Invalid card replacement command");
        if(c.contains("read_fault")&&!c.at("read_fault").is_boolean())
            throw std::invalid_argument("Invalid media card read fault command");
        if(c.at("version")!=1||!c.at("inserted").is_boolean()||!c.at("token").is_string())
            throw std::invalid_argument("Invalid media card command");
        auto token=c.at("token").get<std::string>();
        if(token.empty()||token.size()>96)throw std::invalid_argument("Invalid media card command token");
        return token;
    }
    bool fresh(uint64_t now)const{return !report.empty()&&now>=seen&&now-seen<=staleAfterMs;}
    std::optional<CardMatchAdmission> matchAdmission(uint64_t now)const{
        // Legacy clients have no management telemetry: use their guest report.
        if(report.empty())return std::nullopt;
        if(!fresh(now))return CardMatchAdmission::unavailable;
        if(!report.at("configured").get<bool>()||!report.at("inserted").get<bool>()||
           (command&&!command->at("inserted").get<bool>()))return CardMatchAdmission::missing;
        if(report.at("save_failed").get<bool>())return CardMatchAdmission::unavailable;
        if((command&&command->value("read_fault",false))||report.value("read_fault",false)||report.at("units").is_null())
            return CardMatchAdmission::unreadable;
        return report.at("units").get<int32_t>()==0?CardMatchAdmission::empty:CardMatchAdmission::allowed;
    }
    void observe(const J& r,uint64_t now){
        validateReport(r);
        if(!fresh(now)||r.at("save_failed").get<bool>()||
            (!r.at("configured").get<bool>()&&(!command||!command->contains("image_path"))))command.reset();
        report=r;seen=now;
        if(command&&r.at("command_token")==command->at("token"))command.reset();
    }
    bool request(bool inserted,uint64_t now){
        if(!fresh(now)||!report.at("configured").get<bool>()||report.at("save_failed").get<bool>()||command)return false;
        command=J{{"version",1},{"token",epoch+"/"+std::to_string(++sequence)},{"inserted",inserted}};
        return true;
    }
    bool requestReadFault(bool fault,uint64_t now){
        // Presence remains independent; old clients without this capability
        // must not acknowledge an operation they cannot apply.
        if(!fresh(now)||!report.at("configured").get<bool>()||report.at("save_failed").get<bool>()||
           !report.contains("read_fault")||command)return false;
        command=J{{"version",1},{"token",epoch+"/"+std::to_string(++sequence)},
            {"inserted",report.at("inserted")},{"read_fault",fault}};
        return true;
    }
    J pending(uint64_t now)const{return fresh(now)&&command?*command:J(nullptr);}
    bool requestImage(const std::string& path,uint64_t now){
        if(path.empty()||path.size()>4096||path.find('\0')!=std::string::npos||!fresh(now)||
            !report.value("replace_supported",false)||!report.value("can_replace",false)||command)return false;
        command=J{{"version",1},{"token",epoch+"/"+std::to_string(++sequence)},
            {"inserted",false},{"image_path",path}};return true;
    }
    J snapshot(uint64_t now)const{
        auto result=fresh(now)?report:J{{"configured",false},{"inserted",false},{"save_failed",false},{"units",nullptr}};
        result["available"]=fresh(now);result["pending"]=fresh(now)&&command.has_value();result["reported_at_ms"]=seen;
        return result;
    }
    void invalidate(){report=J::object();command.reset();seen=0;}
};
}
