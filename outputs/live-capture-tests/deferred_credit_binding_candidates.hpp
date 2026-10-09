#pragma once
#include "local_tcp_probe.hpp"
#include <nlohmann/json.hpp>
#include <set>
namespace diagnostic {
// Context candidates ONLY. Even one candidate is not independently verified.
// No ledger, episode selection, charging, latest-match rule or time-window rule.
inline nlohmann::json deferredCreditBindingCandidates(const LocalTCPProbe::Bytes& request,unsigned side,
    uint64_t reportTime,const nlohmann::json& history){
    using J=nlohmann::json;
    J result={{"mode","review-candidates-only"},{"verified",false},{"debit_enabled",false},
        {"candidates",J::array()},{"status","unresolved"}};
    uint8_t opcode=0;const auto raw=LocalTCPProbe::observedGameResult(request,true,&opcode);
    if(side>1||raw.size()!=84||!history.is_array()){result["status"]="invalid-input";return result;}
    uint32_t game=0;for(unsigned i=4;i<8;++i)game=(game<<8)|raw[i];
    if(game==0xffffffffu||game==0xfffffffeu){result["status"]="unfinished-snapshot-required";return result;}
    const auto phone=xband::registrationPhone(request);
    const auto profile=request.at(xband::registrationOffset(request,43));
    if(phone.empty()||profile>3){result["status"]="invalid-registration";return result;}
    result["reported_game"]=game;result["reporting_profile"]=profile;result["result_opcode"]=opcode;
    std::set<uint64_t> seen;
    for(const auto& row:history){
        if(!row.is_object()||row.value("event",std::string{})!="connected")continue;
        try{
            if(row.at("unix_ms").get<uint64_t>()>reportTime||row.at("game")!=game)continue;
            const auto& participants=row.at("participants");
            if(!participants.is_array()||participants.size()!=2)continue;
            bool sides[2]{};bool matching=false,valid=true;
            for(const auto& p:participants){
                const int endpoint=p.at("side").get<int>();
                if(endpoint<0||endpoint>1||sides[endpoint]||p.at("game")!=game){valid=false;break;}
                sides[endpoint]=true;
                if(unsigned(endpoint)==side&&p.at("phone")==phone&&p.at("profile")==profile)matching=true;
            }
            if(!valid||!matching)continue;
            const auto id=row.at("id").get<uint64_t>();
            if(!seen.insert(id).second){result["status"]="duplicate-history-id";result["candidates"]=J::array();return result;}
            result["candidates"].push_back({{"activity_record_id",id},{"generation",row.at("generation")},
                {"side",side},{"game",game},{"profile",profile},{"phone",phone},
                {"evidence_scope","matching context only; copied history namespace not verified"}});
        }catch(const J::exception&){continue;}
    }
    const auto count=result["candidates"].size();
    result["status"]=count==0?"no-context-candidate":count==1?"single-candidate-needs-review":"ambiguous-needs-review";
    return result;
}
}
