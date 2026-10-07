#pragma once
#include "game_ranking_settings.hpp"
#include <xband/local_phone_policy.hpp>
#include <span>
namespace diagnostic {
// Local custom policy, not the historical XBAND formula or authentication.
// Initial service reports are baseline-only. New reports after a locally
// completed peer call can award once; exact reports are retained for dedup.
class VFPointLedger {
    std::filesystem::path path;
    nlohmann::json state={{"schema",1},{"accounts",nlohmann::json::object()},{"reports",nlohmann::json::object()},{"levels",nlohmann::json::object()}};
    mutable std::mutex mutex;
    static std::string key(const std::string &phone,uint8_t profile){
        if(profile>3)throw std::runtime_error("VF ledger supports profiles 0..3 only");
        std::string digits;
        for(char c:phone){if(c>='0'&&c<='9')digits+=c;else if(c!='-')throw std::runtime_error("Invalid ledger phone");}
        if(digits.empty()||digits.size()>32)throw std::runtime_error("Invalid ledger phone length");
        return digits+":"+std::to_string(profile)+":65539";
    }
    void save(const nlohmann::json &next){
        if(next["accounts"].size()>4096||next["reports"].size()>16384)throw std::runtime_error("VF ledger capacity reached; preserve dedup history");
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        auto pending=path;pending+=L".pending";
        if(std::filesystem::exists(pending))throw std::runtime_error("Preserve pending VF ledger; recovery required");
        {std::ofstream out(pending,std::ios::binary);out<<next.dump(2);out.flush();if(!out)throw std::runtime_error("VF ledger write failed");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("VF ledger backup failed");}
        if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("VF ledger commit failed");
        state=next;
    }
public:
    struct Receipt {uint32_t points;bool awarded;bool duplicate;};
    explicit VFPointLedger(std::filesystem::path file):path(std::move(file)){
        auto pending=path;pending+=L".pending";
        if(std::filesystem::exists(pending))throw std::runtime_error("Preserve pending VF ledger; recovery required");
        if(std::filesystem::exists(path)){
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read VF ledger");
            auto loaded=nlohmann::json::parse(in);
            if(loaded.at("schema")!=1||!loaded.at("accounts").is_object()||!loaded.at("reports").is_object()||loaded["accounts"].size()>4096||loaded["reports"].size()>16384)throw std::runtime_error("Invalid VF ledger schema");
            for(const auto &v:loaded["accounts"]){if(!v.is_number_unsigned()||v.get<uint64_t>()>999999999)throw std::runtime_error("Invalid VF ledger points");}
            for(const auto &v:loaded["reports"]){if(!v.is_boolean())throw std::runtime_error("Invalid VF ledger receipt");}
            if(!loaded.contains("levels"))loaded["levels"]=nlohmann::json::object(); // Legacy accounts/receipts are preserved.
            if(!loaded["levels"].is_object())throw std::runtime_error("Invalid VF level metadata");
            for(auto it=loaded["levels"].begin();it!=loaded["levels"].end();++it){
                if(!loaded["accounts"].contains(it.key()))throw std::runtime_error("VF level without account");
                const auto fields=it.value().get<std::array<std::string,3>>();
                rankingEUC(fields[0]);rankingEUC(fields[1]);
                if(fields[2].empty()||fields[2].size()>9||fields[2].find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Invalid VF next-level points");
            }
            state=std::move(loaded);
        }
    }
    std::optional<uint32_t> points(const std::string &phone,uint8_t profile)const{
        const auto account=key(phone,profile);std::lock_guard lock(mutex);
        auto it=state["accounts"].find(account);if(it==state["accounts"].end())return {};return it->get<uint32_t>();
    }
    void preserveLegacyLevels(const std::array<std::string,5>& configured){
        // Freeze the previously shared display values per EXISTING account.
        // Never create an account or overwrite saved points/receipts/levels.
        std::lock_guard lock(mutex);auto next=state;bool changed=false;
        for(auto it=next["accounts"].begin();it!=next["accounts"].end();++it){
            if(!next["levels"].contains(it.key())){
                next["levels"][it.key()]=std::array<std::string,3>{configured[1],configured[3],configured[4]};changed=true;
            }
        }
        if(changed)save(next);
    }
    std::array<std::string,5> ranking(const std::string& phone,uint8_t profile,const std::array<std::string,5>& configured)const{
        const auto account=key(phone,profile);std::lock_guard lock(mutex);auto fields=configured;
        const auto it=state["accounts"].find(account);
        fields[2]=it==state["accounts"].end()?"0":std::to_string(it->get<uint32_t>());
        if(it==state["accounts"].end()){
            fields[1]=fields[3]=rankingUTF8(L"\u672a\u767b\u9332");fields[4]="0";
        }else if(state["levels"].contains(account)){
            const auto level=state["levels"][account].get<std::array<std::string,3>>();
            fields[1]=level[0];fields[3]=level[1];fields[4]=level[2];
        } // Existing schema1 accounts without metadata keep their configured levels.
        return fields;
    }
    Receipt observe(const std::string &phone,uint8_t profile,std::span<const uint8_t> result,uint32_t seed,int award,bool eligible){
        if(result.size()!=84||seed>999999999||award<0||award>1)throw std::runtime_error("Invalid VF result/award input");
        auto word=[&](size_t i){return (uint32_t(result[i])<<24)|(uint32_t(result[i+1])<<16)|(uint32_t(result[i+2])<<8)|result[i+3];};
        if(word(0)!=84||word(4)!=65539||word(8)!=0)throw std::runtime_error("Unsupported VF result");
        const auto account=key(phone,profile);std::string report=account+":";
        constexpr char hex[]="0123456789abcdef";
        for(auto b:result){report+=hex[b>>4];report+=hex[b&15];}
        std::lock_guard lock(mutex);auto next=state;
        auto total=next["accounts"].contains(account)?next["accounts"][account].get<uint32_t>():seed;
        if(next["reports"].contains(report))return {total,false,true};
        const uint32_t local=word(12)+word(16),remote=word(20)+word(24);
        const bool awarded=eligible&&local>remote&&award==1;
        if(awarded){if(total==999999999)throw std::runtime_error("VF points overflow");++total;}
        if(!next["accounts"].contains(account))next["levels"][account]=std::array<std::string,3>{rankingUTF8(L"\u672a\u767b\u9332"),rankingUTF8(L"\u672a\u767b\u9332"),"0"};
        next["accounts"][account]=total;next["reports"][report]=awarded;save(next);
        return {total,awarded,false};
    }
};
inline std::shared_ptr<VFPointLedger> activeVFPoints;
}
