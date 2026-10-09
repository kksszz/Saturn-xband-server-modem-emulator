#include "local_tcp_probe.hpp"
#include <iostream>
int main(){try{
    auto check=[](bool yes,const char* message){if(!yes)throw std::runtime_error(message);};
    {
        unsigned classified=0;
        for(unsigned mode=0;mode<19;++mode){
            media_card::ServiceDebitExchange e;e.arm(3,100,true);
            e.request(media_card::CardReport{2,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,0,0,3}},10);
            e.state=media_card::ServiceDebitExchange::State::Completed;e.result=2;
            e.updatedCard=media_card::CardReport{0,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,0,0,0}};
            if(mode==1){e.initialCard->value=0;e.initialCard->raw->back()=0;e.result=0;}
            if(mode==2)e.state=media_card::ServiceDebitExchange::State::Waiting;
            if(mode==3)e.result.reset();
            if(mode==4)e.result=-1;
            if(mode==5)e.result=3;
            if(mode==6)e.result=1;
            if(mode==7)e.initialCard->value=-1;
            if(mode==8)e.updatedCard->value=-1;
            if(mode==9)e.updatedCard->value=1;
            if(mode==10)e.initialCard.reset();
            if(mode==11)e.initialCard->raw.reset();
            if(mode==12)e.updatedCard.reset();
            if(mode==13)e.updatedCard->raw.reset();
            if(mode==14)(*e.updatedCard->raw)[0]^=1;
            if(mode==15)e.updatedCard->raw->back()=1;
            if(mode==16)e.initialCard->raw->back()=7;
            if(mode==17)e.fullCardReply=false;
            if(mode==18){e.requested=4097;e.result=4096;e.initialCard->value=4096;
                e.initialCard->raw->back()=0;(*e.initialCard->raw)[8]=1;}
            check(e.exhaustedShortfall()==(mode==0||mode==1||mode==18),"Exhausted shortfall evidence boundary");
            ++classified;
        }
        std::cout<<"PASS "<<classified<<" exhausted-card classification cases: exact retained evidence, base8 value, negative/absent/contradictory/identity/time-state failures not guessed\n";
    }
    {
        // Explicit diagnostic guard boundaries; no card writes or real service.
        unsigned cases=0;
        for(unsigned mode=0;mode<13;++mode){
            LocalTCPProbe trial;auto& e=trial.cardDebit;
            e.arm(3,100,true);
            e.request(media_card::CardReport{100,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,1,15,15}},10);
            e.state=media_card::ServiceDebitExchange::State::Completed;e.result=3;
            e.updatedCard=media_card::CardReport{97,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,1,15,1}};
            if(mode==1)e.initialCard.reset();
            if(mode==2)e.initialCard->raw.reset();
            if(mode==3)e.updatedCard.reset();
            if(mode==4)e.updatedCard->raw.reset();
            if(mode==5)e.initialCard->value=-1;
            if(mode==6)e.updatedCard->value=-1;
            if(mode==7){e.initialCard->value=-1;e.updatedCard->value=-4;}
            if(mode==8)e.updatedCard->value=100;
            if(mode==9)e.updatedCard->value=96;
            if(mode==10)(*e.updatedCard->raw)[7]^=1;
            if(mode==11)e.fullCardReply=false;
            if(mode==12){e.requested=0;e.result=0;e.updatedCard=e.initialCard;}
            const bool expected=mode==0||mode==12;
            bool blocked=false;try{trial.releaseAfterCardDebitTrial();}catch(const std::exception&){blocked=true;}
            check(blocked!=expected&&trial.cardDebitReleased==expected,"Diagnostic balance/identity guard boundary");
            ++cases;
        }
        std::cout<<"PASS "<<cases<<" diagnostic continuation guard boundaries; missing/negative/contradictory card reports, changed identity, full reply required, exact and zero allowed\n";
    }
    {
        media_card::CardReport report;report.value=100;report.raw=std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,1,15,15};
        for(uint32_t amount:{0u,1u,3u,7u})for(int32_t value:{0,1,3,7,-1,-2,-4})for(unsigned split=0;split<=5;++split){
            media_card::ServiceDebitExchange exchange;exchange.arm(amount,100);
            check(exchange.request(report,10)==media_card::debitRequest(amount,*report.raw),"Native amount/raw encoding changed");
            bool blocked=false;try{exchange.request(report,10);}catch(const std::exception&){blocked=true;}
            check(blocked,"Native debit request repeated");
            LocalTCPProbe::Bytes response{0x1e,0,0,0,0};LocalTCPProbe::putLong(response,1,uint32_t(value));
            exchange.receive(std::span<const uint8_t>(response).first(split),20);
            exchange.receive(std::span<const uint8_t>(response).subspan(split),21);
            check(exchange.state==media_card::ServiceDebitExchange::State::Completed&&exchange.result==value,
                "Fragmented/signed native reply misdecoded");
        }
        // Actual DecAthlete reply: opcode was a separate TCP payload, followed
        // by the refreshed report and the signed result (25 bytes total).
        const LocalTCPProbe::Bytes capturedReply{0x1e,0,0,99,0,0,0,13,1,2,3,4,5,6,7,0,0,0,1,15,7,0,0,0,1};
        for(size_t split=0;split<=capturedReply.size();++split){
            media_card::ServiceDebitExchange actual;actual.arm(1,100,true);actual.request(report,10);
            actual.receive(std::span<const uint8_t>(capturedReply).first(split),20);
            if(split<capturedReply.size())check(!actual.result,"Card report prefix mistaken for debit result");
            actual.receive(std::span<const uint8_t>(capturedReply).subspan(split),21);
            check(actual.result==1&&actual.updatedCard&&actual.updatedCard->value==99&&
                actual.updatedCard->raw->back()==7,"Captured whole Saturn reply misdecoded");
        }
        for(unsigned badLength:{1u,14u,0xffffffffu}){
            auto invalid=capturedReply;LocalTCPProbe::putLong(invalid,4,badLength);
            media_card::ServiceDebitExchange bad;bad.arm(1,100,true);bad.request(report,10);bad.receive(invalid,20);
            check(bad.state==media_card::ServiceDebitExchange::State::Uncertain&&!bad.result,"Invalid report length accepted");
        }
        media_card::ServiceDebitExchange late;late.arm(1,100);late.request(report,10);late.expire(110);
        late.receive(std::array<uint8_t,5>{0x1e,0,0,0,1},111);
        check(late.state==media_card::ServiceDebitExchange::State::Uncertain&&!late.result,"Late reply resumed expired trial");
        std::cout<<"PASS 168 debit codec/fragmentation cases; signed errors; exact deadline/late reply; no repeated request\n";
    }
        {
            using T=LocalTCPProbe;using B=T::Bytes;
            unsigned cases=0;
            for(unsigned cardLength:{0u,13u})for(uint8_t selector:{uint8_t(2),uint8_t(3),uint8_t(4)}){
                // Synthetic extension of the established resource/result framing.
                // The raw card is deliberately opaque and contains misleading tags.
                B request(184+cardLength,0);
                const B prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};
                std::copy(prefix.begin(),prefix.end(),request.begin());
                request[22]=12;
                const std::string phone="33366666665";
                std::copy(phone.begin(),phone.end(),request.begin()+23);
                request[135]=0x1e;request[138]=100;T::putLong(request,139,cardLength);
                if(cardLength){const B raw{0x0e,2,0x1d,0x20,0x15,0xff,0,0,0,0,1,15,15};
                    std::copy(raw.begin(),raw.end(),request.begin()+143);}
                request[143+cardLength]=0x0c;T::putLong(request,144+cardLength,0x10003);
                request[159+cardLength]=0x0e;request[160+cardLength]=selector==2?3:selector;
                request[161+cardLength]=0x1c;request[162+cardLength]=16;
                request[175+cardLength]=2;request[176+cardLength]=1;
                request.insert(request.end(),{0x15,0,2,0,1,0,0,1,0x14});
                request.insert(request.end(),276,0);
                request.insert(request.end(),{2,1,0,0,1,0x44});request.insert(request.end(),324,0);
                request.insert(request.end(),{0x16,0,0,0x1d,0,0,0x20,0,0,0,84});
                B result(84,0);T::putLong(result,0,0x10003);
                request.insert(request.end(),result.begin(),result.end());request.insert(request.end(),4,0);
                request.insert(request.end(),{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
                if(selector==2){const B name{3,'2','2',0};
                    request[160+cardLength]=2;request.insert(request.begin()+161+cardLength,name.begin(),name.end());}
                check(T::completeMailProbeRequest(request),"Opaque card postmatch framing rejected");
                check(T::serviceRequestCode(request)==selector,"Card shifted selector misread");
                check(diagnostic::receivedGameID(request)==0x10003,"Card shifted game ID misread");
                B expectedResult{0,0,0,84};expectedResult.insert(expectedResult.end(),result.begin(),result.begin()+80);
                check(T::observedGameResult(request)==expectedResult,"Card shifted result changed");
                if(selector!=4){check(T::standbyWaitPreference(request)==2,"Card shifted wait preference");
                    check(T::standbyAreaPreference(request)==1,"Card shifted area preference");}
                for(size_t cut=0;cut<request.size();++cut)
                    check(!T::completeMailProbeRequest(B(request.begin(),request.begin()+cut)),"Partial card postmatch accepted");
                for(uint32_t length:{1u,12u,14u,0xffffffffu}){
                    auto bad=request;T::putLong(bad,139,length);
                    check(!T::completeMailProbeRequest(bad),"Unsupported card length accepted");}
                auto bad=request;bad[136]=1;
                check(!T::completeMailProbeRequest(bad),"Unsupported card tag accepted");
                bad=request;bad.push_back(0);
                check(!T::completeMailProbeRequest(bad),"Card request trailing byte accepted");
                {
                    T trial;uint64_t now=10;unsigned prepared=0,served=0;
                    trial.cardDebitClock=[&]{return now;};trial.replyEnd02=true;
                    trial.prepareServiceReply=[&]{++prepared;return true;};
                    trial.onServiceReplySent=[&]{++served;};
                    trial.armCardDebitTrial(3,100);
                    trial.receive(T::packet(100,0,2,{},true));
                    trial.receive(T::packet(101,T::localInitial+1,0x10,{},true));
                    auto sent=trial.receive(T::packet(101,T::localInitial+1,0x18,request,true,0));
                    if(cardLength){
                        check(trial.cardDebit.state==media_card::ServiceDebitExchange::State::Armed&&
                            trial.localNext==T::localInitial+1,"Zero TCP window emitted debit");
                        sent=trial.receive(T::packet(trial.nextGuest,trial.localNext,0x10,{},true,22));
                    }
                    const auto saved=trial.captured;
                    if(!cardLength){
                        check(trial.cardDebit.state==media_card::ServiceDebitExchange::State::Uncertain&&
                            !trial.end02Sent&&prepared==0,"Absent-card trial continued service");
                    }else{
                        check(sent.size()==62&&sent[40]==0x49&&T::longword(sent,41)==3&&
                            T::longword(sent,45)==13&&!trial.end02Sent&&prepared==0,"Trial request must be49 only");
                        check(trial.pollServiceReply().empty(),"Debit request retransmitted");
                        const auto guest=trial.nextGuest,server=trial.localNext;
                        T::Bytes nativeReply{0x1e,0,0,97,0,0,0,13,1,2,3,4,5,6,7,0,0,0,1,15,12,0,0,0,3};
                        std::copy_n(request.begin()+143,8,nativeReply.begin()+8);
                        trial.receive(T::packet(guest,server,0x18,T::Bytes(nativeReply.begin(),nativeReply.begin()+3),true));
                        check(!trial.end02Sent&&!trial.cardDebit.result,"Partial native response completed");
                        const T::Bytes nativeTail(nativeReply.begin()+3,nativeReply.end());
                        trial.receive(T::packet(guest+3,server,0x18,nativeTail,true));
                        check(trial.cardDebit.result==3&&!trial.end02Sent&&trial.captured==saved&&prepared==0,
                            "Native reply changed registration or prematurely continued");
                        // TCP duplicate segment is ACKed, not re-executed or re-appended.
                        trial.receive(T::packet(guest+3,server,0x18,nativeTail,true));
                        check(trial.cardDebit.result==3&&trial.cardDebit.response.size()==25&&
                            trial.cardDebit.updatedCard&&trial.cardDebit.updatedCard->value==97,"Duplicate response repeated");
                        trial.releaseAfterCardDebitTrial();
                        const auto end=trial.pollServiceReply();
                        check(end.size()==41&&end[40]==2&&trial.end02Sent&&prepared==1&&served==1,
                            "Explicit release did not send service terminator once");
                        check(trial.pollServiceReply().empty(),"Service completed twice");
                    }
                    // Fresh sessions, never reuse the card transaction.
                    for(unsigned failure=0;failure<6&&cardLength;++failure){
                        T failed;failed.cardDebitClock=[&]{return now;};failed.replyEnd02=true;
                        failed.armCardDebitTrial(3,100);
                        failed.receive(T::packet(100,0,2,{},true));
                        failed.receive(T::packet(101,T::localInitial+1,0x10,{},true));
                        failed.receive(T::packet(101,T::localInitial+1,0x18,request,true));
                        const auto guest=failed.nextGuest,server=failed.localNext;
                        if(failure==0){now+=100;failed.pollServiceReply();now-=100;}
                        if(failure==1)failed.receive(T::packet(guest,server,0x11,{},true));
                        if(failure==2)failed.receive(T::packet(guest,server,0x14,{},true));
                        if(failure==3)failed.receive(T::packet(guest,server,0x18,{0x1d,0,0,0,3},true));
                        if(failure==4)failed.receive(T::packet(guest,server,0x18,{0x1e,0,0,0,3,0},true));
                        if(failure==5)failed.receive(T::packet(guest,server,0x18,{0x1e,0,0,0,2},true));
                        bool blocked=false;try{failed.releaseAfterCardDebitTrial();}catch(const std::exception&){blocked=true;}
                        check(blocked&&!failed.end02Sent&&failed.captured==request&&failed.pollServiceReply().empty(),
                            "Unknown/partial consumption resumed or retried");
                    }
                    if(cardLength){
                        // Confirmed exhaustion must end with the bounded
                        // rejection, never unlock mail/matching or send49 again.
                        unsigned shortfallCases=0;
                        for(unsigned balance:{0u,1u,2u})for(size_t split=0;split<=25;++split){
                            B partialRequest=request;partialRequest[137]=0;partialRequest[138]=uint8_t(balance);
                            std::fill(partialRequest.begin()+151,partialRequest.begin()+156,0);
                            partialRequest[155]=uint8_t((1u<<balance)-1);
                            B response{0x1e,0,0,0,0,0,0,13};
                            response.insert(response.end(),partialRequest.begin()+143,partialRequest.begin()+151);
                            response.insert(response.end(),{0,0,0,0,0,0,0,0,uint8_t(balance)});
                            T rejected;rejected.replyEnd02=true;rejected.cardDebitClock=[](){return uint64_t(10);};
                            unsigned prepared=0,normal=0,denied=0,noticePrepared=0;
                            rejected.prepareServiceReply=[&]{++prepared;return true;};
                            rejected.onServiceReplySent=[&]{++normal;};
                            const B notice{0x22,0,1,0,0,0,120,1,44,0,0,0,2,'X',0,2};
                            rejected.prepareCardDebitFailureReply=[&](const auto& exchange){
                                check(exchange.exhaustedShortfall(),"Unverified reply classified as exhausted");
                                ++noticePrepared;return notice;};
                            rejected.onCreditDenialReplySent=[&]{++denied;};
                            rejected.armCardDebitTrial(3,100);
                            rejected.receive(T::packet(100,0,2,{},true));
                            rejected.receive(T::packet(101,T::localInitial+1,0x10,{},true));
                            const auto debit=rejected.receive(T::packet(101,T::localInitial+1,0x18,partialRequest,true));
                            check(debit.size()==62&&debit[40]==0x49,"Shortfall initial49 missing");
                            const auto guest=rejected.nextGuest,server=rejected.localNext;
                            auto feed=[&](size_t offset,size_t count){
                                if(count)rejected.receive(T::packet(guest+uint32_t(offset),server,0x18,
                                    B(response.begin()+offset,response.begin()+offset+count),true,0));};
                            feed(0,split);
                            if(split<25)check(!rejected.creditDenialSent&&!noticePrepared,"Partial reply emitted rejection");
                            feed(split,25-split);
                            check(rejected.cardDebit.exhaustedShortfall()&&!rejected.creditDenialSent&&
                                noticePrepared==1&&rejected.pollServiceReply().empty(),"Zero window emitted/reprepared rejection");
                            const auto packet=rejected.receive(T::packet(rejected.nextGuest,rejected.localNext,0x10,{},true,unsigned(notice.size())));
                            check(packet.size()==40+notice.size()&&std::equal(notice.begin(),notice.end(),packet.begin()+40)&&
                                rejected.creditDenialSent&&!rejected.cardDebitReleased&&denied==1&&prepared==0&&normal==0,
                                "Shortfall reached ordinary service or lacked22/02");
                            rejected.receive(T::packet(guest,server,0x18,response,true));
                            check(rejected.pollServiceReply().empty()&&rejected.cardDebit.response==response&&
                                rejected.cardDebit.result==balance&&rejected.captured==partialRequest&&denied==1,
                                "Duplicate replayed shortfall debit/rejection");
                            ++shortfallCases;
                        }
                        std::cout<<"PASS "<<shortfallCases<<" exhausted-card TCP cases for service "<<unsigned(selector)
                            <<": balances0/1/2, all reply splits, zero window, duplicates,22/02 only, no mail/matching or debit release\n";
                        // Whole Saturn wire replies, not the five-byte body
                        // used above by isolated serializer fixtures. These
                        // are synthetic transport cases, not live card debits
                        // or a reconstruction of historical charge policy.
                        struct Outcome{int32_t result;uint16_t remaining;bool inserted;bool changedIdentity=false;};
                        const Outcome outcomes[]={{0,0,true},{1,0,true},{3,97,true},
                            {-1,0,false},{-2,100,true},{-4,100,true},{4,96,true},
                            {3,100,true},{3,96,true},{3,0,false},{3,97,true,true}};
                        unsigned wholeCases=0;
                        for(const auto outcome:outcomes){
                            B response{0x1e,0,uint8_t(outcome.remaining>>8),uint8_t(outcome.remaining),
                                0,0,0,uint8_t(outcome.inserted?13:0)};
                            if(outcome.inserted){
                                B raw{1,2,3,4,5,6,7,0,0,0,0,0,0};
                                std::copy_n(request.begin()+143,8,raw.begin());
                                if(outcome.changedIdentity)raw[0]^=1;
                                unsigned remaining=outcome.remaining;
                                for(unsigned i=0;i<5;++i){const unsigned weight=1u<<(3*(4-i));
                                    const auto digit=remaining/weight;remaining%=weight;
                                    raw[8+i]=uint8_t((1u<<digit)-1);}
                                response.insert(response.end(),raw.begin(),raw.end());
                            }
                            const auto at=response.size();response.resize(at+4);
                            T::putLong(response,at,uint32_t(outcome.result));
                            for(size_t split=0;split<=response.size();++split){
                                T guarded;guarded.replyEnd02=true;guarded.cardDebitClock=[](){return uint64_t(10);};
                                unsigned afterDebit=0,serviceSent=0;
                                guarded.prepareServiceReply=[&]{++afterDebit;return true;};
                                guarded.onServiceReplySent=[&]{++serviceSent;};
                                guarded.armCardDebitTrial(3,100);
                                guarded.receive(T::packet(100,0,2,{},true));
                                guarded.receive(T::packet(101,T::localInitial+1,0x10,{},true));
                                const auto debit=guarded.receive(T::packet(101,T::localInitial+1,0x18,request,true));
                                check(debit.size()==62&&debit[40]==0x49,"Whole-reply trial did not send exactly49");
                                const auto guest=guarded.nextGuest,server=guarded.localNext;
                                auto feed=[&](size_t offset,size_t count){
                                    if(!count)return;
                                    guarded.receive(T::packet(guest+uint32_t(offset),server,0x18,
                                        B(response.begin()+offset,response.begin()+offset+count),true));
                                };
                                feed(0,split);
                                if(split<response.size())check(!guarded.cardDebit.result,"Whole-reply prefix completed debit");
                                feed(split,response.size()-split);
                                check(guarded.cardDebit.state==media_card::ServiceDebitExchange::State::Completed&&
                                    guarded.cardDebit.result==outcome.result&&guarded.cardDebit.updatedCard&&
                                    guarded.cardDebit.updatedCard->value==outcome.remaining&&
                                    guarded.cardDebit.updatedCard->raw.has_value()==outcome.inserted,
                                    "Whole-reply outcome/report lost");
                                // Duplicate TCP data must not become a new debit or an extra result.
                                guarded.receive(T::packet(guest,server,0x18,response,true));
                                check(guarded.cardDebit.response==response&&guarded.captured==request&&
                                    guarded.pollServiceReply().empty()&&afterDebit==0&&serviceSent==0&&
                                    !guarded.end02Sent,"Whole-reply automatically resumed or retransmitted");
                                bool blocked=false;
                                try{guarded.releaseAfterCardDebitTrial();}catch(const std::exception&){blocked=true;}
                                check(blocked==(outcome.result!=3||outcome.remaining!=97||!outcome.inserted||outcome.changedIdentity),
                                    "Nonmatching result/balance/identity debit was released");
                                if(!blocked){const auto end=guarded.pollServiceReply();
                                    check(end.size()==41&&end[40]==2&&afterDebit==1&&serviceSent==1,
                                        "Matching debit explicit release failed");
                                }else check(guarded.pollServiceReply().empty()&&afterDebit==0&&serviceSent==0,
                                    "Blocked debit reached service or matching callback");
                                ++wholeCases;
                            }
                        }
                        check(wholeCases==260,"Whole-reply matrix count changed");
                        std::cout<<"PASS "<<wholeCases<<" whole-reply transport cases for service "<<unsigned(selector)
                            <<": zero, partial, exact, absent, mismatch, failure, excessive result, contradictory balance and changed identity; every split; duplicate TCP; manual-only continuation\n";
                    }
                }
                ++cases;
            }
            std::cout<<"PASS card postmatch framing: "<<cases<<" synthetic mail/match/named cases, opaque13/absent, selector/game/result/preferences, all truncations and malformed lengths; no automatic charging; native49/1E transport tests\n";
        }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
