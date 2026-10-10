#define PB3_SERVICE_SERVER_TEST 1
#include "pb3_service_server.cpp"
#include <source_location>
namespace {
using J=nlohmann::json;using B=LocalTCPProbe::Bytes;using A=xband::CardMatchAdmission;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("Match card admission assertion line "+std::to_string(at.line()));
}
J report(int units=71,bool inserted=true){return {{"version",1},{"configured",true},{"inserted",inserted},
    {"units",units},{"save_failed",false},{"command_token",""}};}
J control(PB3PairControlEndpoint& endpoint,const J& command){
    for(auto b:command.dump())check(endpoint.transmit(uint8_t(b),0));endpoint.tick(0);
    std::string out;uint8_t b;while(endpoint.peek(b)){out+=char(b);endpoint.consume();}return J::parse(out);
}
B login(unsigned balance,unsigned code=3,uint32_t game=0x10003,bool present=true){
    B b(197,0);const B prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};std::copy(prefix.begin(),prefix.end(),b.begin());
    b[22]=12;const std::string phone="03123456789";std::copy(phone.begin(),phone.end(),b.begin()+23);
    b[81]='T';b[121]=6;b[128]=0x0b;b[135]=0x1e;b[138]=uint8_t(balance);b[137]=uint8_t(balance>>8);
    LocalTCPProbe::putLong(b,139,13);unsigned n=balance;
    for(int i=12;i>=8;--i){b[143+i]=uint8_t((1u<<(n&7))-1);n>>=3;}
    b[156]=0x0c;LocalTCPProbe::putLong(b,157,game);b[172]=0x0e;b[173]=uint8_t(code);b[174]=0x1c;b[175]=16;b[188]=2;b[189]=1;
    b.insert(b.end(),{0x15,0,2,0,1,0,0,1,0x14});b.insert(b.end(),276,0);
    b.insert(b.end(),{2,1,0,0,1,0x44});b.insert(b.end(),324,0);
    b.insert(b.end(),{0x16,0,0,0x1d,0,0});
    b.insert(b.end(),{0x20,0,0,0,84});b.insert(b.end(),84,0);b.insert(b.end(),4,0);
    b.insert(b.end(),{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
    if(code==2)b.insert(b.begin()+174,{3,'2','2',0});
    if(!present){LocalTCPProbe::putLong(b,139,0);b.erase(b.begin()+143,b.begin()+156);}
    check(LocalTCPProbe::completeMailProbeRequest(b));return b;
}
std::unique_ptr<PB3Service> service(const std::shared_ptr<PB3PairControl>& pair,unsigned side,B wire){
    auto s=std::make_unique<PB3Service>(side,std::make_shared<PB3Observation>(),pair->roles,false,false);
    s->setMatchCardAdmission([pair,side]{pair->enforceAdmission();return pair->cardAdmission(side,GetTickCount64());});
    auto& p=s->testTCP();p.captured=std::move(wire);p.state=LocalTCPProbe::State::Established;p.serviceWindow=65535;
    p.gameIntroTitleReply={};p.gameMatchAwardReply={};p.regionTownReply={};p.usageAreaPreferenceReply={};p.usageAreaReply={};
    return s;
}
struct Pair {
    std::shared_ptr<PB3PairControl> p=std::make_shared<PB3PairControl>();
    PB3PairControlEndpoint left{p,0},right{p,1};
    Pair(){p->standbyEnabled=true;
        for(auto* endpoint:{&left,&right})control(*endpoint,{{"op","join"},{"standby_protocol","xband-readonly-v1"},{"media_card",report()}});}
    void observe(unsigned side,J r){control(side?right:left,{{"op","poll"},{"media_card",r}});}
    void wait(unsigned side,uint32_t game=0x10003,const std::string& target={}){
        auto d=p->registerStandby(side,side?"03223456789":"03123456789",game,side?"22":"11",target);
        check(d.result==xband::StandbyRegistration::Result::receiver);
        control(side?right:left,{{"op","standby_ready"},{"generation",p->generation},{"ticket",d.ticket},{"media_card",report()}});
    }
};
}
int main()try{
    for(uint32_t game:{0x10003u,0x10006u,0x18003u,0x7fffffffu})for(unsigned side:{0u,1u}){
        for(unsigned fault:{0u,1u,2u,3u,4u}){
            Pair f;auto r=report();
            if(fault==0)r["inserted"]=false;
            if(fault==1)r["units"]=0;
            if(fault==2)r["units"]=nullptr;
            if(fault==3){r["save_failed"]=true;r["units"]=nullptr;}
            if(fault==4){r["read_fault"]=true;r["units"]=nullptr;}
            f.observe(side,r);auto s=service(f.p,side,login(71,3,game));
            auto packet=s->testTCP().pollServiceReply();check(packet.size()>40&&packet[40]==(fault==3?0x22:0x0a)&&packet.back()==2);
            check(s->testTCP().creditDenialSent&&!f.p->roles->requested[side]&&f.p->roles->caller==2&&f.p->state==0);
            check(s->testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Disabled);
            if(fault!=3)check(s->testTCP().creditDenialReply==diagnostic::originalCardWarningReply(
                fault==0?0x75:fault==1?0x42:0x107));
            bool denied=false;try{f.p->registerStandby(side,side?"03223456789":"03123456789",game);}catch(const PB3CardAdmissionDenied&){denied=true;}check(denied);
            f.wait(1-side,game);check(f.p->roles->caller==2&&!f.p->closing); // Healthy peer remains waiting, never dialled.
            // Restoring a readable, inserted positive card must allow a NEW
            // request. Never resume the refused service or reuse its notice.
            f.observe(side,report());
            const auto recovered=f.p->registerStandby(side,side?"03223456789":"03123456789",game,side?"22":"11");
            check(recovered.result==xband::StandbyRegistration::Result::caller);
            check(f.p->roles->caller==side);
            check(s->testTCP().pollServiceReply().empty()); // Old refusal stays finished.
        }
        for(bool named:{false,true}){
            Pair f;f.wait(side,game,named?(side?"11":"22"):"");f.observe(side,report(71,false));
            check(!f.p->standby.entry(side)&&f.p->roles->caller==2&&!f.p->closing);
            f.wait(1-side,game);check(f.p->roles->caller==2); // Removed waiter cannot be matched.
            f.observe(side,report());auto d=f.p->registerStandby(side,side?"03223456789":"03123456789",game,side?"22":"11");
            check(d.result==xband::StandbyRegistration::Result::caller); // Reinsert + new request recovers.
        }
    }
    for(bool present:{false,true})for(unsigned code:{2u,3u}){
        auto pair=std::make_shared<PB3PairControl>();auto s=service(pair,0,login(0,code,0x10003,present));
        check(s->testTCP().pollServiceReply()[40]==0x0a&&pair->roles->caller==2&&!pair->roles->requested[0]);
    }
    for(unsigned units:{1u,2u}){Pair f;f.observe(0,report(units));auto s=service(f.p,0,login(units));
        check(s->testTCP().pollServiceReply().empty()&&f.p->roles->requested[0]&&!s->testTCP().creditDenialSent);}
    for(bool present:{false,true}){Pair f;f.observe(0,report(0,present));auto s=service(f.p,0,login(0,4,0x10003,present));
        auto packet=s->testTCP().pollServiceReply();check(packet.size()>40&&packet[40]!=0x22&&!s->testTCP().creditDenialSent);}
    {
        Pair f;auto s=service(f.p,0,login(71));auto& p=s->testTCP();
        p.cardDebitReleased=true;p.cardDebit.updatedCard=media_card::CardReport{0,std::array<uint8_t,13>{}};
        check(p.pollServiceReply()[40]==0x0a&&!f.p->roles->requested[0]); // Post-settlement zero, even before next telemetry.
    }
    {
        Pair f;f.wait(0);check(f.p->mediaCards[0].request(false,GetTickCount64()));f.p->enforceAdmission();
        check(!f.p->standby.entry(0)&&!f.p->closing); // Pending ejection blocks immediately.
        auto stale=GetTickCount64()+xband::MediaCardControl::staleAfterMs+1;f.p->enforceCardAdmission(stale);
        check(f.p->cardAdmission(1,stale)==A::unavailable);
    }
    {
        Pair f;f.wait(0);auto d=f.p->registerStandby(1,"03223456789",0x10003,"22");check(d.result==xband::StandbyRegistration::Result::caller);
        auto response=control(f.right,{{"op","dial"},{"generation",f.p->generation},{"number","3336666666"},{"media_card",report(0)}});
        check(response.at("closed")==true&&response.at("state")==0); // Removal/zero raced assignment: never ring peer.
    }
    {
        Pair f;f.wait(0);check(f.p->mediaCards[0].requestReadFault(true,GetTickCount64())==false); // Old capability absent.
        auto good=report();good["read_fault"]=false;f.observe(0,good);
        check(f.p->mediaCards[0].requestReadFault(true,GetTickCount64()));f.p->enforceAdmission();
        check(!f.p->standby.entry(0)&&f.p->roles->caller==2&&!f.p->closing);
    }
    {
        Pair f;f.p->state=2;f.p->roles->caller=0;f.observe(0,report(71,false));
        check(f.p->state==2&&!f.p->closing); // Existing game owns its disconnect/card-error lifecycle.
    }
    std::cout<<"PASS early card admission: no card/zero/unreadable/stale/pending removal, both sides, named/automatic waits, reinsert, pre-dial race, free receiving, 1/2 units, common game IDs\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
