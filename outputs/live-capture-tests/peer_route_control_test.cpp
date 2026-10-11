#include <xband/windows_tcp_host.hpp>
#include "pb3_pair_control.hpp"
#include <iostream>
namespace {
using J=nlohmann::json;
void check(bool value){if(!value)throw std::runtime_error("Peer endpoint route assertion");}
J command(PB3PairControlEndpoint& endpoint,const J& request){
    for(auto b:request.dump())check(endpoint.transmit(uint8_t(b),0));endpoint.tick(0);
    std::string result;uint8_t b;while(endpoint.peek(b)){result+=char(b);endpoint.consume();}
    return J::parse(result);
}
struct Pair {
    std::shared_ptr<PB3PairControl> state=std::make_shared<PB3PairControl>();
    PB3PairControlEndpoint left{state,0},right{state,1};
    explicit Pair(unsigned caller){
        command(left,{{"op","join"}});command(right,{{"op","join"}});
        state->roles->caller=caller;
        state->activityContext[0]["phone"]="0312345678";
        state->activityContext[1]["phone"]="0312345679";
    }
    PB3PairControlEndpoint& endpoint(unsigned side){return side?right:left;}
};
}
int main()try{
    for(unsigned side:{0u,1u}){
        for(bool legacy:{false,true}){
            Pair pair(side);J request{{"op","dial"},{"generation",pair.state->generation}};
            if(legacy)request["number"]=xband::legacyPeerPhone(side);
            else request["target"]=xband::pairedPeerEndpoint(side);
            check(command(pair.endpoint(side),request)["state"]==1);
            check(command(pair.endpoint(1-side),{{"op","answer"},{"generation",pair.state->generation}})["state"]==2);
        }
        for(unsigned variant=0;variant<7;++variant){
            Pair pair(side);J request{{"op","dial"},{"generation",pair.state->generation},{"target",xband::pairedPeerEndpoint(side)}};
            unsigned actor=side;
            if(variant==0)request["target"]=xband::peerEndpoint(side);
            if(variant==1)request["target"]="peer-2";
            if(variant==2)request.erase("generation");
            if(variant==3)request["generation"]=pair.state->generation+1;
            if(variant==4)request["number"]=xband::legacyPeerPhone(side);
            if(variant==5)actor=1-side;
            if(variant==6)pair.state->joined[1-side]=false;
            bool denied=false;try{command(pair.endpoint(actor),request);}catch(const std::runtime_error&){denied=true;}
            check(denied&&pair.state->state==0);
        }
        Pair stale(side);
        check(command(stale.endpoint(side),{{"op","dial"},{"generation",0},{"target",xband::pairedPeerEndpoint(side)}})["state"]==0);
    }
    std::cout<<"PASS endpoint peer routing both directions; arbitrary account phones; legacy compatibility; role, generation, membership, target and ambiguous-route rejection\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
