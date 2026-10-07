#pragma once
#include <stdexcept>
#include <array>
namespace xband {
// Local two-player matchmaking policy. Only a complete supported service
// request may select roles, never endpoint join or PPP/TCP establishment.
struct LocalMatchRoles {
    unsigned caller=2; // 2 = not assigned yet
    unsigned first=2;
    bool blocked=false;
    std::array<bool,2> requested{};
    unsigned accept(unsigned side){
        if(side>1)throw std::invalid_argument("Invalid matchmaking side");
        if(blocked)return 2;
        if(first==2)first=side;
        requested[side]=true;
        if(caller==2&&requested[0]&&requested[1])caller=first;
        return caller;
    }
    void withdraw(unsigned side){
        if(side>1)throw std::invalid_argument("Invalid matchmaking side");
        if(caller<2)return; // Established roles belong to the call lifecycle.
        requested[side]=false;
        first=requested[1-side]?1-side:2;
    }
    unsigned callee()const{return caller<2?1-caller:2;}
};
}
