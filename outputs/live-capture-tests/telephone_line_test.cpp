#include <xband/windows_modem_service.hpp>
#include <objidl.h>
#include <xband/modem_command_session.hpp>
#include "pb3_pair_control.hpp"
#define XBAND_DASHBOARD_RENDER_TEST 1
#include "xband_dashboard.hpp"
#include <chrono>
#include <iostream>

namespace {
void check(bool ok){if(!ok)throw std::runtime_error("Telephone line assertion");}
struct Echo: xband::ServiceEndpoint {
    std::deque<uint8_t> bytes;
    bool transmit(uint8_t b,unsigned)override{bytes.push_back(b);return true;}
    void tick(unsigned)override{}
    bool peek(uint8_t& b)const override{if(bytes.empty())return false;b=bytes.front();return true;}
    void consume()override{bytes.pop_front();}
    size_t pending()const override{return bytes.size();}
    void reset()override{bytes.clear();}
};
nlohmann::json request(PB3PairControlEndpoint& endpoint,nlohmann::json value){
    for(auto b:value.dump())check(endpoint.transmit(uint8_t(b),0));
    endpoint.tick(0);std::string text;uint8_t b;
    while(endpoint.peek(b)){text+=char(b);endpoint.consume();}
    return nlohmann::json::parse(text);
}
}
int main(int argc,char** argv)try{
    XbandDashboard::testButtonRepaintIsolation();
    {
        using namespace xband::monitor;
        CallTimer timer;
        check(timer.display(0)==L"通話時間：--:--:--（未接続）");
        timer.observe(CallKind::service,1,1000);
        check(timer.active()&&timer.display(66000)==L"通話時間：00:01:05（サーバー）");
        timer.observe(CallKind::service,2,66000); // Peer generations do not restart service calls.
        check(timer.milliseconds(67000)==66000);
        timer.observe(CallKind::none,2,67000);
        check(!timer.active()&&timer.display(999999)==L"前回通話：00:01:06（サーバー）");
        timer.observe(CallKind::peer,2,100000);
        timer.observe(CallKind::peer,2,110000);check(timer.milliseconds(112000)==12000);
        timer.observe(CallKind::peer,3,112000);check(timer.milliseconds(112000)==0);
        check(timer.display(3773000)==L"通話時間：01:01:01（対戦）");
        check(timer.milliseconds(1)==0); // No unsigned wrap on malformed earlier observation.
        nlohmann::json view={{"endpoints",nlohmann::json::array({{{"connected",true},{"call_active",false}},{{"connected",true},{"call_active",true}}})},
            {"pair_control",{{"state",0},{"caller",2},{"joined",{true,true}},{"failed",false}}}};
        auto links=connectionView(view,{true,true});
        check(links.service[0]==ServiceLink::ready&&links.service[1]==ServiceLink::calling&&links.peer==PeerLink::idle);
        view["pair_control"]["state"]=1;view["pair_control"]["caller"]=0;
        check(connectionView(view,{true,true}).peer==PeerLink::dialingLeft);
        view["pair_control"]["caller"]=1;check(connectionView(view,{true,true}).peer==PeerLink::dialingRight);
        view["pair_control"]["state"]=2;check(connectionView(view,{true,true}).peer==PeerLink::connected);
        links=connectionView(view,{true,false});check(links.peer==PeerLink::disconnected&&links.service[1]==ServiceLink::disconnected);
        view["pair_control"]["failed"]=true;check(connectionView(view,{true,true}).peer==PeerLink::disconnected);
        view["pair_control"]["failed"]=false;view["pair_control"]["joined"]={true,false};
        check(connectionView(view,{true,true}).peer==PeerLink::idle);
        if(argc==2){
            // Synthetic display fixtures only: never touch running calls/cards.
            auto& p=view["pair_control"];p["generation"]=1;p["transport"]="async-v1";
            p["sent"]={0,0};p["received"]={0,0};p["requested"]={false,false};p["joined"]={true,true};
            for(auto& endpoint:view["endpoints"])endpoint["pb3_observation"]={{"subscriber",""},{"received","0"},{"sent","0"}};
            const auto directory=std::filesystem::path(argv[1]);
            p["state"]=0;p["caller"]=2;
            XbandDashboard::renderSnapshot(view,directory/"service.png");
            p["state"]=1;p["caller"]=0;
            XbandDashboard::renderSnapshot(view,directory/"dial-left.png");
            p["caller"]=1;XbandDashboard::renderSnapshot(view,directory/"dial-right.png");
            p["state"]=2;XbandDashboard::renderSnapshot(view,directory/"peer.png");
            p["caller"]=0;XbandDashboard::renderSnapshot(view,directory/"peer-left.png");
            XbandDashboard::renderSnapshot(view,directory/"line-off.png",{true,false});
        }
    }
    auto pair=std::make_shared<PB3PairControl>();pair->standbyEnabled=true;
    PB3PairControlEndpoint left(pair,0),right(pair,1);
    request(left,{{"op","join"},{"transport","async-v1"},{"standby_protocol","xband-readonly-v1"}});
    request(right,{{"op","join"},{"transport","async-v1"},{"standby_protocol","xband-readonly-v1"}});
    pair->registerStandby(0,"0312345678",0x10006);
    pair->setTelephoneLine(0,false);
    check(!pair->telephoneLine[0]&&pair->telephoneLine[1]&&!pair->standby.entry(0));
    check(pair->joined[0]&&pair->joined[1]&&!pair->failed);
    auto state=request(left,{{"op","poll"}});
    check(state.at("telephone_line")==nlohmann::json::array({false,true}));
    bool rejected=false;try{pair->registerStandby(0,"0312345678",0x10006);}catch(...){rejected=true;}check(rejected);
    pair->setTelephoneLine(0,true);pair->roles->caller=0;pair->state=2;
    unsigned ended=0;pair->creditCarrierEnded=[&]{++ended;};
    pair->setTelephoneLine(1,false);pair->setTelephoneLine(1,false);
    check(ended==1&&pair->closing&&pair->state==0&&!pair->failed);
    const auto generation=pair->generation;
    request(left,{{"op","closed_ack"},{"generation",generation}});
    request(right,{{"op","closed_ack"},{"generation",generation}});
    check(pair->generation==generation+1&&!pair->closing&&!pair->telephoneLine[1]);
    pair->setTelephoneLine(1,true);check(pair->state==0); // ON cannot resume an old call.
    // A rebooted/lost endpoint ends the old carrier without poisoning a join.
    pair->roles->caller=0;pair->state=2;
    const auto lostGeneration=pair->generation;left.reset();
    check(!pair->failed&&pair->closing&&pair->closedAck[0]&&ended==2);
    auto rejoin=request(left,{{"op","join"},{"transport","async-v1"},{"standby_protocol","xband-readonly-v1"}});
    check(rejoin.at("closed")==true&&rejoin.at("recoverable")==true);
    check(pair->registerStandby(0,"0312345678",0x10006).result==xband::StandbyRegistration::Result::defer);
    request(right,{{"op","closed_ack"},{"generation",lostGeneration}});
    check(pair->generation==lostGeneration+1&&!pair->closing&&pair->joined[0]&&pair->joined[1]);
    pair->registerStandby(0,"0312345678",0x10006);
    check(pair->standby.entry(0).has_value());

    using Host=xband::windows::TcpHost;using Client=xband::windows::ModemServiceClient;
    unsigned calls=0;
    Host host({"127.0.0.1",58240,false,{{"test",std::string(64,'a')}}},xband::FrameClock(1,1),
        [&](uint64_t){++calls;return std::make_unique<Echo>();},Host::PortMode::loopback_ephemeral_test);
    const xband::windows::ClientConfig config{"127.0.0.1",host.port(),false,"test",std::string(64,'a'),60};
    const auto pump=[&](Client& client,auto done){
        const auto until=GetTickCount64()+5000;
        while(GetTickCount64()<until){check(host.step(GetTickCount64()));const bool ok=client.step(GetTickCount64());
            if(done(ok))return;Sleep(1);}
        throw std::runtime_error("Telephone line socket timeout");
    };
    Client original(config,GetTickCount64());pump(original,[&](bool ok){check(ok);return original.state()==Client::State::idle;});
    check(original.open("0001",GetTickCount64()));pump(original,[&](bool ok){check(ok);return original.carrier();});
    check(calls==1);host.setAccepting(false);
    pump(original,[](bool ok){return !ok;});check(host.clientCount()==0);
    Client offline(config,GetTickCount64());pump(offline,[](bool ok){return !ok;});check(calls==1);
    host.setAccepting(true);
    Client fresh(config,GetTickCount64());pump(fresh,[&](bool ok){check(ok);return fresh.state()==Client::State::idle;});
    check(!fresh.carrier()&&calls==1);check(fresh.open("0001",GetTickCount64()));
    pump(fresh,[&](bool ok){check(ok);return fresh.carrier();});check(calls==2);
    std::cout<<"PASS independent UI switches, repaint isolation, OFF rejection, carrier loss, ON fresh call\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
