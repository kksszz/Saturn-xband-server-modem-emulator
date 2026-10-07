#pragma once
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
namespace xband::monitor {
// Read-only diagnostic side channel. Never used by modem or service logic.
inline std::string testProgressLine(const nlohmann::json &v){
    if(!v.is_object()||!v.contains("scope")||!v["scope"].is_string()||v["scope"]!="vf-mail-dual-diagnostic"||
       !v.contains("stage")||!v["stage"].is_number_integer()||!v.contains("state")||!v["state"].is_string())return "TEST CONTROLLER: unknown report";
    if(v["stage"]<0||v["stage"]>5)return "TEST CONTROLLER: unknown report";
    const int stage=v["stage"].get<int>();const auto state=v["state"].get<std::string>();
    if(state=="passed"&&stage==5)return "TEST CONTROLLER: PASS - strict log / snapshot checks; inspect game images separately";
    if(state=="failed")return "TEST CONTROLLER: FAIL - inspect result logs (not a modem status)";
    if(state!="running")return "TEST CONTROLLER: unknown report";
    static const char *steps[]={"waiting for both guests", "modem 1 initial send", "modem 2 send / receive", "both second calls", "reconnect / held inventory", "finishing / strict checks"};
    return "TEST CONTROLLER: "+std::to_string(stage)+"/5 - "+steps[stage];
}
inline std::string readTestProgressLine(const char *path){
    if(!path||!*path)return {};
    try{
        std::ifstream in(path,std::ios::binary);char buffer[1025]{};in.read(buffer,sizeof(buffer));
        const auto count=in.gcount();if(!in.eof()||count<1||count>1024)return "TEST CONTROLLER: report unavailable";
        return testProgressLine(nlohmann::json::parse(buffer,buffer+count));
    }catch(...){return "TEST CONTROLLER: report unavailable";}
}
}
