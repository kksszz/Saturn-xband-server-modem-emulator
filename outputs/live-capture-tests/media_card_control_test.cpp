#include <xband/windows_modem_service.hpp>
#include <objidl.h>
#include <xband/virtual_media_card.hpp>
#include "pb3_pair_control.hpp"
#define XBAND_DASHBOARD_RENDER_TEST 1
#include "xband_dashboard.hpp"
#include <source_location>
#include <iostream>
namespace {
using J=nlohmann::json;
void check(bool ok,std::source_location where=std::source_location::current()){
    if(!ok)throw std::runtime_error("Card controls assertion line "+std::to_string(where.line()));
}
J request(PB3PairControlEndpoint& endpoint,const J& value){
    for(auto b:value.dump())check(endpoint.transmit(uint8_t(b),0));endpoint.tick(0);
    std::string wire;uint8_t b;while(endpoint.peek(b)){wire+=char(b);endpoint.consume();}return J::parse(wire);
}
J report(int units=100){return {{"version",1},{"configured",true},{"inserted",true},{"units",units},{"read_fault",false},{"save_failed",false},{"command_token",""}};}
}
int main(int argc,char** argv)try{
    diagnostic::MediaCardExchangeWindow::testControls();
    {
        const auto root=std::filesystem::temp_directory_path()/("xband-new-card-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        std::filesystem::create_directory(root);const auto path=root/"new.bin";
        diagnostic::createSyntheticCard(path,50);
        check(std::filesystem::file_size(path)==13);
        bool refused=false;try{diagnostic::createSyntheticCard(path,100);}catch(...){refused=true;}check(refused);
        std::ifstream input(path,std::ios::binary);xband::VirtualMediaCard saved;
        input.read(reinterpret_cast<char*>(saved.data.data()),13);check(saved.remainingUnits()==50);
    }
    for(unsigned units:{0u,50u,100u,32767u}){
        xband::VirtualMediaCard generated;generated.data=diagnostic::syntheticCard(units);
        check(generated.remainingUnits()==int32_t(units));
    }
    bool invalidUnits=false;try{diagnostic::syntheticCard(32768);}catch(...){invalidUnits=true;}check(invalidUnits);
    {
        xband::MediaCardControl exchange;auto r=report(0);
        r["replace_supported"]=true;r["can_replace"]=false;exchange.observe(r,10);
        check(!exchange.requestImage("C:/card.bin",10));
        r["inserted"]=false;r["can_replace"]=true;exchange.observe(r,11);
        check(exchange.requestImage("C:/card.bin",11));
        const auto c=exchange.pending(11);check(c.at("inserted")==false&&c.at("image_path")=="C:/card.bin");
        check(!exchange.requestImage("C:/other.bin",12));xband::MediaCardControl::validateCommand(c);
        r["command_token"]=c.at("token");r["units"]=100;exchange.observe(r,12);
        check(exchange.pending(12).is_null()&&exchange.snapshot(12).at("units")==100);
        check(!exchange.requestImage("C:/card.bin",2013));
    }
    xband::VirtualMediaCard card;
    for(unsigned units:{0u,1u,2u,71u,96u,100u,32767u}){
        card.data.fill(0);auto remainder=units;
        for(int i=12;i>=8;--i){card.data[i]=uint8_t((1u<<(remainder&7))-1);remainder>>=3;}
        const auto raw=card.data;card.setInserted(true);check(card.remainingUnits()==int32_t(units));
        card.setInserted(false);check(card.remainingUnits()==int32_t(units)&&card.data==raw);
        card.setInserted(true);check(card.data==raw);
    }
    card.data.fill(255);check(!card.remainingUnits());card.data[0]=0;check(!card.remainingUnits());
    card.data.fill(0);card.data[12]=7;card.setInserted(true);const auto intact=card.data;
    card.setReadFault(true);check(card.present&&!card.remainingUnits()&&(card.read()&0x11)==0x11);
    for(auto pin:{0x8d,0x81,0x89,0x81,0x85,0x81})card.write(uint8_t(pin));check(card.data==intact);
    card.setInserted(false);check(!(card.read()&0x10));card.setInserted(true);check(card.readFault);
    card.setReadFault(false);check(card.remainingUnits()==3&&card.data==intact);
    xband::MediaCardControl channel;check(!channel.request(false,100));
    auto telemetry=report(71);channel.observe(telemetry,100);check(channel.snapshot(100).at("units")==71);
    check(channel.request(false,101));const auto command=channel.pending(101);check(!channel.request(true,102));
    check(channel.pending(200).at("token")==command.at("token"));
    auto wrong=telemetry;wrong["command_token"]="old";channel.observe(wrong,201);check(!channel.pending(201).is_null());
    telemetry["inserted"]=false;telemetry["command_token"]=command.at("token");channel.observe(telemetry,202);
    check(channel.pending(202).is_null()&&channel.snapshot(202).at("units")==71);
    check(!channel.snapshot(2203).at("available").get<bool>()&&channel.snapshot(2203).at("units").is_null());
    check(!channel.request(true,2203));channel.invalidate();check(!channel.snapshot(202).at("available").get<bool>());
    channel.observe(report(),3000);check(channel.request(false,3001));channel.observe(report(),5001);
    check(channel.pending(5001).is_null()); // Do not replay an old switch after a stale/paused connection.
    check(channel.requestReadFault(true,5002));check(channel.matchAdmission(5002)==xband::CardMatchAdmission::unreadable);
    auto faulty=report();faulty["read_fault"]=true;faulty["units"]=nullptr;faulty["command_token"]=channel.pending(5002).at("token");
    channel.observe(faulty,5003);check(channel.pending(5003).is_null()&&channel.matchAdmission(5003)==xband::CardMatchAdmission::unreadable);
    faulty["inserted"]=false;channel.observe(faulty,5004);check(channel.matchAdmission(5004)==xband::CardMatchAdmission::missing);
    check(channel.requestReadFault(false,5005));faulty["read_fault"]=false;faulty["units"]=100;
    faulty["command_token"]=channel.pending(5005).at("token");channel.observe(faulty,5006);check(channel.snapshot(5006).at("units")==100);
    auto legacy=report();legacy.erase("read_fault");channel.observe(legacy,5007);check(!channel.requestReadFault(true,5007));
    bool rejected=false;try{auto invalid=report(-1);channel.observe(invalid,300);}catch(...){rejected=true;}check(rejected);
    auto pair=std::make_shared<PB3PairControl>();PB3PairControlEndpoint left(pair,0),right(pair,1);
    auto l=report(71),r=report(96);request(left,{{"op","join"},{"media_card",l}});request(right,{{"op","join"},{"media_card",r}});
    pair->setTelephoneLine(0,false);check(pair->mediaCards[0].request(false,GetTickCount64()));
    auto reply=request(left,{{"op","poll"},{"media_card",l}});const auto token=reply.at("media_card_command").at("token");
    check(reply.at("telephone_line")[0]==false);check(request(right,{{"op","poll"},{"media_card",r}}).at("media_card_command").is_null());
    l["inserted"]=false;l["command_token"]=token;check(request(left,{{"op","poll"},{"media_card",l}}).at("media_card_command").is_null());
    check(pair->mediaCards[0].snapshot(GetTickCount64()).at("units")==71&&pair->mediaCards[1].snapshot(GetTickCount64()).at("units")==96);
    check(pair->mediaCards[0].request(true,GetTickCount64()));left.reset();check(pair->mediaCards[0].pending(GetTickCount64()).is_null());
    // Legacy adapter: no report => controls disabled, no guessed zero balance.
    check(request(left,{{"op","join"}}).at("media_card_command").is_null());
    XbandDashboard::testMediaCardControls();XbandDashboard::testButtonRepaintIsolation();
    if(argc==2){
        J observation={{"subscriber",""},{"received","0"},{"sent","0"}};
        J endpoint={{"connected",false},{"call_active",false},{"pb3_observation",observation}};
        J view={{"endpoints",J::array({endpoint,endpoint})},{"pair_control",{{"state",0},{"failed",false},{"caller",2},{"joined",{true,true}},{"requested",{false,false}},{"generation",1},{"transport","async-v1"},{"sent",{0,0}},{"received",{0,0}}}}};
        xband::MediaCardControl a,b;auto first=report(71),second=report(96);second["inserted"]=false;
        first["read_fault"]=true;first["units"]=nullptr;
        a.observe(first,GetTickCount64());b.observe(second,GetTickCount64());view["media_cards"]=J::array({a.snapshot(GetTickCount64()),b.snapshot(GetTickCount64())});
        XbandDashboard::renderSnapshot(view,std::filesystem::path(argv[1]));
    }
    std::cout<<"PASS card controls: units codec, persistent balance, independent endpoints, OFF-line operation, acknowledgements, stale/legacy disabling and UI layout\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
