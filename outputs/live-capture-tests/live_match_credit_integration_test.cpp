#define PB3_SERVICE_SERVER_TEST 1
#include "pb3_service_server.cpp"
#include <iostream>
#include <source_location>

namespace {
using J=nlohmann::json;using B=LocalTCPProbe::Bytes;
using Match=diagnostic::LiveMatchCredits;
using Ledger=diagnostic::DeferredCreditSettlement;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("Live match credit assertion line "+std::to_string(at.line()));
}
template<class F> void rejects(F fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed);}
media_card::CardReport card(unsigned balance,unsigned side){
    check(balance<=32767);std::array<uint8_t,13> raw{uint8_t(side+1),2,3,4,5,6,7,0,0,0,0,0,0};
    auto units=balance;for(unsigned i=13;i-->8;){raw[i]=uint8_t((1u<<(units%8))-1);units/=8;}
    return {int32_t(balance),raw};
}
B reply(unsigned requested,const media_card::CardReport& initial){
    const auto actual=std::min(requested,unsigned(initial.value));const auto updated=card(unsigned(initial.value)-actual,(*initial.raw)[0]-1);
    B wire{0x1e,0,uint8_t(updated.value>>8),uint8_t(updated.value),0,0,0,13};
    wire.insert(wire.end(),updated.raw->begin(),updated.raw->end());
    wire.insert(wire.end(),{uint8_t(actual>>24),uint8_t(actual>>16),uint8_t(actual>>8),uint8_t(actual)});
    return wire;
}
B request(unsigned side,unsigned balance,uint32_t game=0x10003,int32_t error=0,bool won=true,
          unsigned profile=0,uint8_t selector=4){
    B wire(197,0);const B prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};std::copy(prefix.begin(),prefix.end(),wire.begin());
    wire[22]=12;const std::string phone=side?"03223456789":"03123456789";
    std::copy(phone.begin(),phone.end(),wire.begin()+23);wire[43]=uint8_t(profile);wire[81]='T';
    const auto c=card(balance,side);wire[135]=0x1e;wire[137]=uint8_t(balance>>8);wire[138]=uint8_t(balance);
    LocalTCPProbe::putLong(wire,139,13);std::copy(c.raw->begin(),c.raw->end(),wire.begin()+143);
    wire[156]=0x0c;LocalTCPProbe::putLong(wire,157,game);wire[172]=0x0e;wire[173]=selector;
    wire[174]=0x1c;wire[175]=16;wire[188]=2;wire[189]=1;
    wire.insert(wire.end(),{0x15,0,2,0,1,0,0,1,0x14});wire.insert(wire.end(),276,0);
    wire.insert(wire.end(),{2,1,0,0,1,0x44});wire.insert(wire.end(),324,0);
    wire.insert(wire.end(),{0x16,0,0,0x1d,0,0});
    B raw(84,0);LocalTCPProbe::putLong(raw,0,84);LocalTCPProbe::putLong(raw,4,game);
    LocalTCPProbe::putLong(raw,8,uint32_t(error));
    LocalTCPProbe::putLong(raw,won?12:20,1);
    if(error){wire.insert(wire.end(),{0x21,0x23});wire.insert(wire.end(),raw.begin(),raw.end());}
    else {
        wire.push_back(0x20);wire.insert(wire.end(),raw.begin(),raw.end());
        wire.insert(wire.end(),8,0);wire.push_back(0x24);
    }
    wire.insert(wire.end(),{0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
    check(LocalTCPProbe::completeMailProbeRequest(wire,true));return wire;
}
std::array<J,2> contexts(unsigned balance,uint32_t game=0x10003){
    std::array<J,2> result;
    for(unsigned side=0;side<2;++side){
        const auto c=card(balance,side);
        result[side]={{"phone",side?"03223456789":"03123456789"},{"profile",0},{"game",game},
            {"credit_baseline",J::array()},{"credit_card",{{"value",balance},{"raw",*c.raw}}}};
    }
    return result;
}
}

namespace {
J control(PB3PairControlEndpoint& endpoint,const J& command){
    for(uint8_t b:command.dump())check(endpoint.transmit(b,0));
    endpoint.tick(0);std::string output;uint8_t b;
    while(endpoint.peek(b)){output.push_back(char(b));endpoint.consume();}
    return J::parse(output);
}
void configureProbe(LocalTCPProbe& p,const B& wire){
    p.captured=wire;p.state=LocalTCPProbe::State::Established;p.serviceWindow=65535;
    p.gameIntroTitleReply={};p.gameMatchAwardReply={};p.regionTownReply={};
    p.usageAreaPreferenceReply={};p.usageAreaReply={};p.cardDebitClock=[](){return uint64_t(0);};
}
B initialRequest(unsigned side,unsigned balance,uint32_t game=0x10003){
    auto wire=request(side,balance,game,0,true,0,3);
    // Structurally known empty-counter result, frozen before this carrier.
    const size_t result=wire.size()-12-1-8-84;
    LocalTCPProbe::putLong(wire,result+12,0);
    const auto raw=LocalTCPProbe::observedGameResult(wire,true);
    check(raw.size()==84&&LocalTCPProbe::longword(raw,12)==0);return wire;
}
struct Fixture{
    std::shared_ptr<diagnostic::ServiceCreditSettings> settings;
    std::shared_ptr<Ledger> ledger;
    std::shared_ptr<Match> matches;
    std::shared_ptr<PB3PairControl> pair=std::make_shared<PB3PairControl>();
    PB3PairControlEndpoint left{pair,0},right{pair,1};
    unsigned balance;
    Fixture(const std::filesystem::path& dir,unsigned value,bool enabled=true,uint32_t game=0x10003,
            unsigned normal=3,unsigned mail=1):balance(value){
        settings=std::make_shared<diagnostic::ServiceCreditSettings>(dir/"settings.json");
        settings->save(mail,normal,true,enabled);
        ledger=std::make_shared<Ledger>(dir/"debits.json");
        matches=std::make_shared<Match>(dir/"matches.json");
        installLiveMatchCreditHooks(pair,matches,settings);
        control(left,{{"op","join"}});control(right,{{"op","join"}});
        std::array<std::unique_ptr<PB3Service>,2> registrations;
        for(unsigned side=0;side<2;++side){
            registrations[side]=std::make_unique<PB3Service>(side,std::make_shared<PB3Observation>(),pair->roles,false,false);
            auto& service=*registrations[side];
            service.setServiceCredits(settings,ledger);service.setMatchCredits(matches);
            // Optional history is intentionally absent. Billing still freezes context.
            service.setActivityHistory({},[this,side](const J& c){pair->activityContext[side]=c;return pair->generation;});
            configureProbe(service.testTCP(),initialRequest(side,balance,game));
            (void)service.testTCP().pollServiceReply(); // First side waits for peer.
            check(pair->roles->requested[side]); // Actual production matching preparation.
        }
        check(pair->roles->caller==0);
        control(left,{{"op","dial"},{"number","3336666665"},{"generation",pair->generation}});
        control(right,{{"op","answer"},{"generation",pair->generation}});
        check(pair->state==2);
    }
    void end(bool transport=false){
        if(transport)left.reset();
        else control(left,{{"op","hangup"},{"generation",pair->generation}});
        if(settings->snapshot().matchEnabled)check(matches->snapshot().at("match/1").at("phase")=="ended");
    }
    std::unique_ptr<PB3Service> service(unsigned side,const B& wire){
        auto s=std::make_unique<PB3Service>(side,std::make_shared<PB3Observation>(),pair->roles,false,false);
        s->setServiceCredits(settings,ledger);s->setMatchCredits(matches);
        s->setActivityHistory({},[this,side](const J& c){pair->activityContext[side]=c;return pair->generation;});
        configureProbe(s->testTCP(),wire);return s;
    }
};
void nativeDebit(PB3Service& s,unsigned balance,unsigned side,unsigned amount,bool denied,
                 unsigned& prepared,unsigned& ordinary){
    auto& p=s.testTCP();const auto prepare=p.prepareServiceReply;const auto onSent=p.onServiceReplySent;
    p.prepareServiceReply=[&,prepare]{++prepared;return prepare();};
    p.onServiceReplySent=[&,onSent]{++ordinary;if(onSent)onSent();};
    p.serviceWindow=21;check(p.pollServiceReply().empty());
    check(p.cardDebit.state==media_card::ServiceDebitExchange::State::Armed);
    p.serviceWindow=65535;const auto packet=p.pollServiceReply();
    check(packet.size()==62&&packet[40]==0x49&&LocalTCPProbe::longword(packet,41)==amount);
    check(p.pollServiceReply().empty()&&prepared==0&&ordinary==0);
    p.cardDebit.receive(reply(amount,card(balance,side)),0);
    const auto result=p.pollServiceReply();check(!result.empty());
    check(denied?p.creditDenialSent&&result[40]==0x22&&result.back()==2&&!prepared&&!ordinary:
        !p.creditDenialSent&&p.cardDebitReleased&&prepared==1&&ordinary==1);
    check(p.pollServiceReply().empty()); // Same TCP call never repeats49 or ordinary service.
}
}
int main(){try{
    const auto root=std::filesystem::temp_directory_path()/("xband-live-integration-"+
        std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    unsigned cases=0;
    for(bool reset:{false,true})for(unsigned balance:{0u,1u,2u,3u,4u,10u}){
        Fixture f(root/(std::string(reset?"reset-":"normal-")+std::to_string(balance)),balance);
        check(f.matches->snapshot().at("match/1").at("participants").at(0).at("card").at("value")==balance);
        f.end();
        const auto leftWire=request(0,balance,0x10003,reset?-608:0,true);
        const auto rightWire=request(1,balance,0x10003,reset?-609:0,false);
        // Each side consumes on its FIRST access, before the peer reports.
        // No manual consume/continue UI and no extra reconnect.
        const unsigned match=reset?1:3,amount=match+(balance>=match+1?1:0);
        const bool denied=balance<match+1;
        for(unsigned side:{0u,1u}){
            auto s=f.service(side,side?rightWire:leftWire);unsigned prep=0,ordinary=0;
            nativeDebit(*s,balance,side,amount,denied,prep,ordinary);
            const auto row=f.ledger->snapshot().at("match/1/side:"+std::to_string(side));
            check(row.at("state")==(balance<amount?"exhausted":"confirmed"));
            check(row.at("consumed")==std::min(balance,amount)&&
                row.at("updated_card").at("value")==balance-std::min(balance,amount));
            const auto participant=f.matches->snapshot().at("match/1").at("participants").at(side);
            check(participant.at("settled")==true&&participant.at("paid")==(balance>=amount));
            check(f.pair->activityContext[side].at("credit_card").at("value")==balance-std::min(balance,amount));
            if(side==0){
                check(f.matches->snapshot().at("match/1").at("reports").at(1).is_null());
                check(!f.ledger->snapshot().contains("match/1/side:1"));
            }
            ++cases;
        }
        auto reopened=Match(root/(std::string(reset?"reset-":"normal-")+std::to_string(balance))/"matches.json");
        check(reopened.snapshot()==f.matches->snapshot());++cases;
    }
    // Regression for the reported 100 -> 96 asymmetry, in BOTH access orders.
    // The exact same production carrier/result/debit path applies to all
    // registered IDs and an unknown future ID, with one global rate setting.
    for(uint32_t game:{0x10003u,0x18003u,0x18002u,0x10007u,0x10006u,0x10004u,
                       0x18001u,0x10005u,0x10008u,0x18004u,0x01020304u})
    for(unsigned first:{0u,1u})for(bool reset:{false,true}){
        Fixture f(root/("common-"+std::to_string(game)+"-"+std::to_string(first)+"-"+std::to_string(reset)),
            20,true,game,7,2);
        f.end();
        const unsigned amount=reset?3:9; // reset1/normal7 plus common mail2.
        for(unsigned side:{first,1-first}){
            auto s=f.service(side,request(side,20,game,reset?(side?-609:-608):0,side==0));
            unsigned prep=0,ordinary=0;nativeDebit(*s,20,side,amount,false,prep,ordinary);
            const auto receipt=f.ledger->snapshot().at("match/1/side:"+std::to_string(side));
            check(receipt.at("consumed")==amount&&receipt.at("updated_card").at("value")==20-amount);
            if(side==first)check(f.ledger->snapshot().size()==1&&
                f.matches->snapshot().at("match/1").at("reports").at(1-first).is_null());
        }
        check(f.matches->snapshot().at("match/1").at("normal_units")==7);
        check(f.matches->snapshot().at("match/1").at("comparison")==
            (reset?"local-and-peer-reset":"normal-reciprocal-results"));
        ++cases;
    }
    // The first receipt is durable while the peer has never contacted us;
    // a later conflicting/unknown peer report cannot revise that debit.
    for(unsigned first:{0u,1u})for(bool unknown:{false,true}){
        Fixture f(root/("independent-first-"+std::to_string(first)+"-"+std::to_string(unknown)),100);f.end();
        auto s=f.service(first,request(first,100));unsigned prep=0,ordinary=0;
        nativeDebit(*s,100,first,4,false,prep,ordinary);
        const auto key="match/1/side:"+std::to_string(first);
        const auto receipt=f.ledger->snapshot().at(key);
        check(receipt.at("updated_card").at("value")==96&&f.ledger->snapshot().size()==1);
        check(f.matches->snapshot().at("match/1").at("reports").at(1-first).is_null());
        f.matches=std::make_shared<Match>(root/("independent-first-"+std::to_string(first)+"-"+std::to_string(unknown))/"matches.json");
        auto peer=f.service(1-first,request(1-first,100,0x10003,unknown?-607:0));
        if(unknown)check(peer->testTCP().pollServiceReply()[40]==0x22);
        else{unsigned peerPrep=0,peerOrdinary=0;nativeDebit(*peer,100,1-first,4,false,peerPrep,peerOrdinary);}
        check(f.ledger->snapshot().at(key)==receipt);
        check(f.matches->snapshot().at("match/1").at("comparison")=="unresolved-or-conflicting-reports");
        // An ordinary subsequent mail access costs only mail1, never match3.
        auto again=f.service(first,request(first,96));unsigned againPrep=0,againOrdinary=0;
        nativeDebit(*again,96,first,1,false,againPrep,againOrdinary);
        check(f.ledger->snapshot().at(key)==receipt);++cases;
    }
    // Default match-OFF stays non-billing even with mail policy active.
    {
        Fixture f(root/"disabled",10,false);check(f.matches->snapshot().empty());f.end();
        auto s=f.service(0,request(0,10));unsigned prep=0,ordinary=0;
        nativeDebit(*s,10,0,1,false,prep,ordinary);
        check(f.matches->snapshot().empty()&&f.ledger->snapshot().size()==1);++cases;
    }
    // Carrier loss is NOT automatically classified as reset1.
    {
        Fixture f(root/"transport",10);f.end(true);
        auto s=f.service(0,request(0,10,0x10003,-607));
        const auto response=s->testTCP().pollServiceReply();
        check(!response.empty()&&response[40]==0x22&&f.ledger->snapshot().empty());++cases;
    }
    // Rate freezes at start. Current settings do not rewrite old normal3.
    {
        Fixture f(root/"frozen",10);f.settings->save(2,9,true,true);f.end();
        auto first=f.service(0,request(0,10));unsigned firstPrep=0,firstOrdinary=0;
        nativeDebit(*first,10,0,5,false,firstPrep,firstOrdinary);
        auto second=f.service(1,request(1,10,0x10003,0,false));unsigned prep=0,ordinary=0;
        nativeDebit(*second,10,1,5,false,prep,ordinary);
        check(f.matches->snapshot().at("match/1").at("normal_units")==3);++cases;
    }
    // Owner/profile mutation, incomplete request, and missing ledger fail closed.
    {
        Fixture f(root/"blocked",10);f.end();
        auto changed=f.service(0,request(0,10,0x10003,0,true,1));
        check(changed->testTCP().pollServiceReply()[40]==0x22&&f.ledger->snapshot().empty());
        auto partial=f.service(0,request(0,10));partial->testTCP().captured.pop_back();
        check(partial->testTCP().pollServiceReply().empty()&&f.ledger->snapshot().empty());
        auto missing=f.service(0,request(0,10));missing->setMatchCredits({});
        check(missing->testTCP().pollServiceReply()[40]==0x22&&f.ledger->snapshot().empty());++cases;
    }
    // A timeout is durable uncertain. New login explains block, never reissues49.
    // Settle on a matchmaking login, then establish the next real carrier.
    // Its frozen cards must reflect the native debit within those same logins.
    {
        Fixture f(root/"next-carrier",10);f.end();
        control(f.left,{{"op","closed_ack"},{"generation",f.pair->generation}});
        control(f.right,{{"op","closed_ack"},{"generation",f.pair->generation}});
        auto first=f.service(0,request(0,10,0x10003,0,true,0,3));
        auto& left=first->testTCP();check(left.pollServiceReply()[40]==0x49);
        left.cardDebit.receive(reply(3,card(10,0)),0);
        (void)left.pollServiceReply();check(left.cardDebitReleased);
        auto second=f.service(1,request(1,10,0x10003,0,false,0,3));
        auto& right=second->testTCP();check(right.pollServiceReply()[40]==0x49);
        right.cardDebit.receive(reply(3,card(10,1)),0);
        (void)right.pollServiceReply();check(right.cardDebitReleased);
        (void)left.pollServiceReply();check(f.pair->roles->caller==0);
        control(f.left,{{"op","dial"},{"number","3336666665"},{"generation",f.pair->generation}});
        control(f.right,{{"op","answer"},{"generation",f.pair->generation}});
        const auto next=f.matches->snapshot().at("match/2");
        check(next.at("phase")=="open"&&next.at("participants").at(0).at("card").at("value")==7&&
              next.at("participants").at(1).at("card").at("value")==7);
        check(f.ledger->snapshot().size()==2);++cases;
    }
    {
        Fixture f(root/"moved-owner",10);f.end();
        auto moved=f.service(1,request(0,10));
        check(moved->testTCP().pollServiceReply()[40]==0x22&&f.ledger->snapshot().empty());
        auto renamed=request(0,10);renamed[24]='9';
        auto changed=f.service(0,renamed);
        check(changed->testTCP().pollServiceReply()[40]==0x22&&f.ledger->snapshot().empty());++cases;
    }
    {
        Fixture f(root/"timeout",10);f.end();
        auto second=f.service(1,request(1,10,0x10003,0,false));auto& p=second->testTCP();
        check(p.pollServiceReply()[40]==0x49);p.cardDebitClock=[](){return uint64_t(60000);};
        check(p.pollServiceReply().empty());second.reset();
        auto again=f.service(1,request(1,10,0x10003,0,false));
        check(again->testTCP().pollServiceReply()[40]==0x22);
        check(f.ledger->snapshot().at("match/1/side:1").at("state")=="uncertain");++cases;
    }
    rejects([]{(void)diagnostic::creditNoticeReply(std::wstring(129,L'A'));});
    rejects([]{(void)diagnostic::creditNoticeReply(std::wstring(L"a\0b",3));});
    std::cout<<"PASS "<<cases<<" production integration scenarios; independent first-access settlement, carrier hooks, no-history, native debit, partial0..3, additive mail, reset both, opt-in, no replay; synthetic only\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
