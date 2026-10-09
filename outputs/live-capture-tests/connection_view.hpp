#pragma once
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace xband::monitor {
enum class ServiceLink { disconnected, ready, calling };
enum class PeerLink { idle, dialingLeft, dialingRight, connected, disconnected };
struct ConnectionView {
    std::array<ServiceLink,2> service{};
    PeerLink peer=PeerLink::idle;
};
enum class CallKind { none, service, peer };
// Monotonic wall time, from the monitor's first connected observation. No fee,
// game-result, inactivity timeout or guest clock decisions depend on this.
class CallTimer {
public:
    void observe(CallKind kind,uint64_t generation,uint64_t now){
        if(kind==CallKind::none){
            if(active_){duration_=now>=start_?now-start_:0;active_=false;}
            return;
        }
        if(!active_||kind!=kind_||(kind==CallKind::peer&&generation!=generation_)){
            start_=now;duration_=0;kind_=kind;generation_=generation;active_=true;
        }
    }
    bool active()const{return active_;}
    CallKind kind()const{return kind_;}
    uint64_t milliseconds(uint64_t now)const{return active_?(now>=start_?now-start_:0):duration_;}
    std::wstring display(uint64_t now)const{
        if(kind_==CallKind::none)return L"通話時間：--:--:--（未接続）";
        const auto seconds=milliseconds(now)/1000;
        std::wostringstream out;
        out<<(active_?L"通話時間：":L"前回通話：")<<std::setfill(L'0')<<std::setw(2)<<seconds/3600
           <<L":"<<std::setw(2)<<(seconds/60)%60<<L":"<<std::setw(2)<<seconds%60
           <<(kind_==CallKind::peer?L"（対戦）":L"（サーバー）");
        return out.str();
    }
private:
    bool active_=false;
    CallKind kind_=CallKind::none;
    uint64_t start_=0,duration_=0,generation_=0;
};
// Service socket registration is not a guest telephone call. Control sockets
// are deliberately excluded from this guest-facing view.
inline ConnectionView connectionView(const nlohmann::json& snapshot,
                                     const std::array<bool,2>& line) {
    ConnectionView result;
    const auto& pair=snapshot.at("pair_control");
    for(unsigned i=0;i<2;++i){
        const auto& endpoint=snapshot.at("endpoints").at(i);
        result.service[i]=!line[i]||!endpoint.value("connected",false)?ServiceLink::disconnected:
            endpoint.value("call_active",false)?ServiceLink::calling:ServiceLink::ready;
    }
    const auto state=pair.value("state",0u),caller=pair.value("caller",2u);
    const auto joined=pair.value("joined",std::array<bool,2>{});
    if(!line[0]||!line[1]||pair.value("failed",false))result.peer=PeerLink::disconnected;
    else if(joined[0]&&joined[1]&&state==2&&caller<2)result.peer=PeerLink::connected;
    else if(joined[0]&&joined[1]&&state==1&&caller<2)
        result.peer=caller==0?PeerLink::dialingLeft:PeerLink::dialingRight;
    return result;
}
}
