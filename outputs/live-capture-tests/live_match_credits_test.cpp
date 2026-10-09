#include "live_match_credits.hpp"
#include "service_mail_credit.hpp"
#include "credit_insufficient_reply.hpp"
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
void endEpisode(Match& matches,const std::string& key){
    matches.end(key);
    // Do not pre-create claims: each caller's access freezes its own mail fee.
}
}
int main(){try{
    const auto root=std::filesystem::temp_directory_path()/("xband-live-match-credit-"+
        std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    unsigned cases=0;
    // Normal3 and common reset1 on BOTH endpoints, with independently
    // additive mail. Low balances remain admitted; the49 reply proves actuals.
    for(bool reset:{false,true})for(unsigned balance:{0u,1u,2u,3u,4u,10u}){
        const auto dir=root/(std::string(reset?"reset-":"normal-")+std::to_string(balance));
        Match matches(dir/"matches.json");auto ledger=std::make_shared<Ledger>(dir/"debits.json");
        const auto key=matches.begin(7,contexts(balance),{true,3,1,1});check(key=="match/1");
        endEpisode(matches,key);
        const unsigned match=reset?1:3,expected=match+(balance>=match+1?1:0);
        for(unsigned side=0;side<2;++side){
            const auto wire=request(side,balance,0x10003,reset?(side?-609:-608):0,side==0);
            const auto plan=matches.observeAndPlan(side,wire,1,true,*ledger);
            check(plan.state==Match::Plan::State::Ready&&plan.amount==expected&&plan.mailDenied==(balance<match+1));
            auto debit=std::make_shared<diagnostic::ServiceMailCredit>(ledger,contexts(balance)[side]["phone"].get<std::string>(),
                "connection-"+std::to_string(side),plan.amount,card(balance,side),
                [&](const char*,const J& receipt){
                    if(receipt.at("state")=="confirmed"||receipt.at("state")=="exhausted")matches.recordReceipt(key,side,*ledger);
                },plan.debitKey,true);
            LocalTCPProbe probe;probe.captured=wire;probe.state=LocalTCPProbe::State::Established;probe.serviceWindow=65535;probe.replyEnd02=true;
            probe.cardDebitClock=[](){return uint64_t(0);};unsigned ordinary=0;
            probe.prepareServiceReply=[&]{++ordinary;return true;};
            probe.prepareCardDebitFailureReply=[](const auto& exchange){return diagnostic::creditInsufficientReply(exchange.requested,0,exchange.result);};
            debit->attach(probe);
            const auto sent=probe.pollServiceReply();check(sent.size()==62&&sent[40]==0x49);
            check(probe.pollServiceReply().empty());probe.cardDebit.receive(reply(expected,card(balance,side)),0);
            const auto continuation=probe.pollServiceReply();
            const auto receipt=ledger->snapshot().at(plan.debitKey);
            const auto actual=std::min(balance,expected);
            check(receipt.at("amount")==expected&&receipt.at("consumed")==actual&&receipt.at("updated_card").at("value")==balance-actual);
            const auto participant=matches.snapshot().at(key).at("participants").at(side);
            check(participant.at("settled")==true&&participant.at("paid")== (balance>=expected));
            if(balance<expected){
                check(receipt.at("state")=="exhausted"&&participant.at("settlement_state")=="exhausted");
                check(!continuation.empty()&&continuation[40]==0x22&&continuation.back()==2&&probe.creditDenialSent&&!probe.cardDebitReleased&&ordinary==0);
            }else{check(receipt.at("state")=="confirmed"&&probe.cardDebitReleased&&ordinary==1);}
            check(probe.pollServiceReply().empty());
            rejects([&]{ledger->issue(plan.debitKey,"replay",card(balance,side));});
        }
        Match restored(dir/"matches.json");Ledger restoredDebits(dir/"debits.json");
        for(unsigned side=0;side<2;++side)
            check(restored.observeAndPlan(side,request(side,balance-std::min(balance,expected)),1,true,restoredDebits).state==Match::Plan::State::None);
        // Exhausted participants are terminal even though full payment was NOT
        // recorded. A later zero-card match can be represented without replay.
        check(restored.begin(8,contexts(0),{true,3,1,1})=="match/2");cases+=2;
    }
    // Admission is explicit; policy OFF neither writes nor examines old history.
    // Registered IDs and an unregistered future ID all take the same policy.
    // IDs below are fixture data, not a production title allowlist.
    for(uint32_t game:{0x10003u,0x18003u,0x18002u,0x10007u,0x10006u,0x10004u,
                       0x18001u,0x10005u,0x10008u,0x18004u,0x01020304u})
    for(unsigned first:{0u,1u})for(int32_t reportError:{0,-608,-609}){
        const auto dir=root/("common-"+std::to_string(game)+"-"+std::to_string(first)+"-"+std::to_string(reportError));
        Match m(dir/"matches.json");Ledger d(dir/"debits.json");
        const auto key=m.begin(1,contexts(20,game),{true,7,1,1});m.end(key);
        const unsigned fee=reportError?1:7;
        for(unsigned side:{first,1-first}){
            const auto error=side==first?reportError:reportError==0?0:reportError==-608?-609:-608;
            const auto plan=m.observeAndPlan(side,request(side,20,game,error,side==0),2,true,d);
            check(plan.state==Match::Plan::State::Ready&&plan.amount==fee+2&&!plan.mailDenied);
            check(m.snapshot().at(key).at("fees").at(side)==fee);
            if(side==first)check(m.snapshot().at(key).at("reports").at(1-first).is_null());
        }
        check(m.snapshot().at(key).at("comparison")==
            (reportError?"local-and-peer-reset":"normal-reciprocal-results"));
        check(d.snapshot().empty());++cases;
    }
    {Match m(root/"off.json");check(m.begin(1,{},{}).empty()&&!std::filesystem::exists(root/"off.json"));++cases;}
    // Stale/unknown reports stay unresolved only on their own endpoint;
    // a fresh normal report does not require a reciprocal peer result.
    for(unsigned mode=0;mode<6;++mode){
        const auto dir=root/("unresolved-"+std::to_string(mode));Match m(dir/"matches.json");Ledger d(dir/"debits.json");
        const uint32_t game=mode==4?0x10005:0x10003;auto c=contexts(10,game);
        if(mode==0)c[0]["credit_baseline"]=LocalTCPProbe::observedGameResult(request(0,10,game));
        const auto key=m.begin(1,c,{true,3,1,1});m.end(key);
        const auto a=request(0,10,game,mode==3?-610:mode==4?-608:0);
        const auto b=request(1,10,game,mode==3?-611:mode==4?-609:0,mode==5);
        const bool unresolved=mode==0||mode==3;
        check(m.observeAndPlan(0,a,1,true,d).state==(unresolved?Match::Plan::State::Waiting:Match::Plan::State::Ready));
        if(mode!=1&&mode!=2)check(m.observeAndPlan(1,b,1,true,d).state==
            (mode==3?Match::Plan::State::Waiting:Match::Plan::State::Ready));
        if(mode==2){auto changed=a;const auto raw=LocalTCPProbe::observedGameResult(a);
            const auto found=std::search(changed.begin(),changed.end(),raw.begin(),raw.end());
            LocalTCPProbe::putLong(changed,size_t(found-changed.begin())+16,1);
            check(m.observeAndPlan(0,changed,1,true,d).state==Match::Plan::State::Blocked);
            check(m.observeAndPlan(1,request(1,10,game,0,false),1,true,d).state==Match::Plan::State::Ready);
        }
        check(d.snapshot().empty());++cases;
    }
    // Peer reports are audit-only: never mutate an already frozen claim,
    // even if results conflict, or if the server restarts before peer access.
    for(unsigned first:{0u,1u}){
        const auto dir=root/("independent-"+std::to_string(first));Match m(dir/"matches.json");Ledger d(dir/"debits.json");
        const auto key=m.begin(1,contexts(100),{true,3,1,1});m.end(key);
        const auto p=m.observeAndPlan(first,request(first,100),1,true,d);
        check(p.state==Match::Plan::State::Ready&&p.amount==4);
        check(m.snapshot().at(key).at("reports").at(1-first).is_null());
        const auto claim=m.snapshot().at(key).at("participants").at(first).at("claim");
        Match restored(dir/"matches.json");
        check(restored.observeAndPlan(first,request(first,100),7,true,d).amount==4);
        check(restored.observeAndPlan(1-first,request(1-first,100),1,true,d).amount==4);
        const auto row=restored.snapshot().at(key);
        check(row.at("comparison")=="unresolved-or-conflicting-reports"&&row.at("participants").at(first).at("claim")==claim);
        ++cases;
    }
    // End marker, frozen profile/account/game/card, and unique episode context.
    // Open old schema1 episodes without rewriting paid receipts or frozen
    // claims. A formerly waiting single report now resolves locally.
    for(bool oldReady:{false,true}){
        const auto dir=root/(oldReady?"legacy-ready":"legacy-ended");const auto path=dir/"matches.json";
        Match m(path);Ledger d(dir/"debits.json");const auto key=m.begin(1,contexts(10),{true,3,1,1});m.end(key);
        auto episodes=m.snapshot();auto& row=episodes[key];
        row["reports"][0]={{"opcode",0x20},{"raw",LocalTCPProbe::observedGameResult(request(0,10))}};
        row.erase("fees");row.erase("outcomes");row.erase("comparison");
        if(oldReady){
            row["reports"][1]={{"opcode",0x20},{"raw",LocalTCPProbe::observedGameResult(request(1,10,0x10003,0,false))}};
            row["phase"]="ready";row["fees"]=J::array({3,3});row["outcome"]="normal-reciprocal-results";
            row["participants"][0]["claim"]={{"amount",5},{"match_units",3},{"mail_units",2},{"mail_denied",false},
                {"card",{{"value",10},{"raw",*card(10,0).raw}}}};
        }
        {std::ofstream out(path,std::ios::binary);out<<J{{"schema",1},{"kind","live-match-credit-episodes"},{"next",uint64_t(2)},{"episodes",episodes}}.dump(2);}
        Match restored(path);const auto p=restored.observeAndPlan(0,request(0,10),1,true,d);
        check(p.state==Match::Plan::State::Ready&&p.amount==(oldReady?5:4));
        check(restored.observeAndPlan(1,request(1,10,0x10003,0,false),1,true,d).amount==4);++cases;
    }
    for(unsigned mode=0;mode<6;++mode){
        const auto dir=root/("binding-"+std::to_string(mode));Match m(dir/"matches.json");Ledger d(dir/"debits.json");
        auto c=contexts(10);const auto key=m.begin(1,c,{true,3,1,1});
        if(mode==0)check(m.observeAndPlan(0,request(0,10),1,true,d).state==Match::Plan::State::Waiting);
        else{
            m.end(key);auto r=request(0,10,0x10003,0,true,mode==1?1:0);
            if(mode==2)LocalTCPProbe::putLong(r,157,0x10005);
            if(mode==3)r[143]^=1;
            if(mode==4)r[138]=9; // reported scalar contradicts the bit card
            if(mode<=4)check(m.observeAndPlan(0,r,1,true,d).state==Match::Plan::State::Blocked);
            else{std::swap(c[0],c[1]);rejects([&]{m.begin(2,c,{true,3,1,1});});}
        }
        check(d.snapshot().empty());++cases;
    }
    // Required fee is frozen at carrier start, zero rates and mail-OFF are
    // explicit policies; a matching duplicate report cannot change a claim.
    for(unsigned normal:{0u,3u,7u})for(bool mailEnabled:{false,true}){
        const auto dir=root/("rates-"+std::to_string(normal)+"-"+std::to_string(mailEnabled));
        Match m(dir/"matches.json");Ledger d(dir/"debits.json");const auto key=m.begin(1,contexts(10),{true,normal,1,1});
        endEpisode(m,key);const auto r=request(0,10);
        const auto p=m.observeAndPlan(0,r,2,mailEnabled,d);
        check(p.state==Match::Plan::State::Ready&&p.amount==normal+(mailEnabled?2:0)&&!p.mailDenied);
        if(p.amount)check(m.observeAndPlan(0,r,7,!mailEnabled,d).amount==p.amount);
        else check(m.observeAndPlan(0,r,2,mailEnabled,d).state==Match::Plan::State::None);
        ++cases;
    }
    // A confirmed native debit survives a crash before the match receipt mark.
    // Recovery synchronizes it once, never reissues49 or charges that access twice.
    for(unsigned balance:{2u,3u,10u}){
        const auto dir=root/("recovery-"+std::to_string(balance));Match m(dir/"matches.json");Ledger d(dir/"debits.json");
        const auto key=m.begin(1,contexts(balance),{true,3,1,1});endEpisode(m,key);
        const auto p=m.observeAndPlan(0,request(0,balance),1,true,d);
        d.queue(p.debitKey,"03123456789",p.amount,card(balance,0),true);d.issue(p.debitKey,"before-crash",card(balance,0));
        check(d.acknowledge(p.debitKey,"before-crash",reply(p.amount,card(balance,0)))==(balance>=p.amount));
        Match restored(dir/"matches.json");Ledger receipts(dir/"debits.json");
        const auto r=request(0,balance-std::min(balance,p.amount));
        const auto recovered=restored.observeAndPlan(0,r,1,true,receipts);
        check(recovered.state==Match::Plan::State::Ready&&recovered.amount==0&&recovered.mailDenied==p.mailDenied);
        check(restored.observeAndPlan(0,r,1,true,receipts).state==Match::Plan::State::None);++cases;
    }
    // No-reply restart cannot automatically repeat a possibly consumed request.
    {const auto dir=root/"uncertain";Match m(dir/"matches.json");Ledger d(dir/"debits.json");
     const auto key=m.begin(1,contexts(10),{true,3,1,1});endEpisode(m,key);
     const auto p=m.observeAndPlan(0,request(0,10),1,true,d);d.queue(p.debitKey,"03123456789",p.amount,card(10,0),true);
     d.issue(p.debitKey,"lost",card(10,0));auto reopened=std::make_shared<Ledger>(dir/"debits.json");
     check(reopened->snapshot().at(p.debitKey).at("state")=="uncertain");
     rejects([&]{auto retry=std::make_shared<diagnostic::ServiceMailCredit>(reopened,"03123456789","new",p.amount,card(10,0),
         std::function<void(const char*,const J&)>{},p.debitKey,true);});++cases;}
    // Invalid or conflicting files do not get silently merged or repaired.
    {const auto path=root/"stale.json";Match a(path),b(path);a.begin(1,contexts(10),{true,3,1,1});
     rejects([&]{b.begin(2,contexts(10),{true,3,1,1});});check(Match(path).snapshot().size()==1);++cases;}
    {const auto path=root/"interrupted.json";std::ofstream temp(path.string()+".tmp");temp<<"partial";temp.close();
     rejects([&]{Match m(path);});++cases;}
    // Default ledger behavior remains sufficient-only and unconfirmed partial
    // Bounded complete-request validation precedes all card offset parsing.
    {Match m(root/"partial-request-matches.json");Ledger d(root/"partial-request-debits.json");
     const auto wire=request(0,10);
     for(size_t length=0;length<wire.size();++length){B partial(wire.begin(),wire.begin()+length);
         check(m.observeAndPlan(0,partial,1,true,d).state==Match::Plan::State::Blocked);}
     check(d.snapshot().empty());++cases;}
    // A spoofed updated bit card cannot mark the match receipt as full payment.
    {const auto dir=root/"contradictory-card";Match m(dir/"matches.json");Ledger d(dir/"debits.json");
     const auto key=m.begin(1,contexts(10),{true,3,1,1});endEpisode(m,key);
     const auto p=m.observeAndPlan(0,request(0,10),1,true,d);d.queue(p.debitKey,"03123456789",p.amount,card(10,0),true);
     d.issue(p.debitKey,"native",card(10,0));auto wrong=reply(p.amount,card(10,0));wrong[20]=0;
     check(d.acknowledge(p.debitKey,"native",wrong));rejects([&]{m.recordReceipt(key,0,d);});
     check(m.snapshot().at(key).at("participants").at(0).at("settled")==false);++cases;}
    // Default ledger behavior remains sufficient-only and unconfirmed partial
    // replies remain blocked unless this specific policy opted into exhaustion.
    {Ledger d(root/"default.json");d.queue("old","owner",3,card(2,0));
     rejects([&]{d.issue("old","connection",card(2,0));});++cases;}
    std::cout<<"PASS "<<cases<<" synthetic live-match credit scenarios: normal3/reset1 both, additive mail, low-balance partial receipts, identity/fresh-report binding, native proof, persistent no-replay recovery\n";
    std::cout<<"Scope: offline component + TCP49/22/02 exchange; production carrier/result hooks and automatic-match GUI switch are NOT activated by this test\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
