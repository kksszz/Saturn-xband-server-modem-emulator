#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace diagnostic {
// Display only: never substitute virtual routing addresses for subscriber data.
inline std::string modemSubscriberDisplay(const nlohmann::json& observation){
    if(!observation.value("verified",false))return "Not yet reported";
    const auto phone=observation.value("subscriber",std::string{});
    return phone.empty()?"Not yet reported":phone;
}
inline std::string modemRouteDisplay(const nlohmann::json& endpoints,unsigned caller){
    if(caller>1)return "Route not assigned\nWaiting for game request";
    const unsigned callee=1-caller;
    return "Game-request route: "+std::to_string(caller+1)+" -> "+std::to_string(callee+1)+
        "\nCaller "+modemSubscriberDisplay(endpoints.at(caller).at("pb3_observation"))+
        "\nTarget "+modemSubscriberDisplay(endpoints.at(callee).at("pb3_observation"));
}
}
