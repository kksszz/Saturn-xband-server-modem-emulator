#define PB3_SERVICE_SERVER_TEST 1
#include "pb3_service_server.cpp"
#include <fstream>
int main(int argc,char** argv){try{
    using J=nlohmann::json;using namespace diagnostic;
    auto check=[](bool v){if(!v)throw std::runtime_error("Automatic mail credit assertion");};
    check(argc==1||argc==2);LocalTCPProbe::Bytes request;
    if(argc==2){std::ifstream input(argv[1]);request=J::parse(input).at("request").get<LocalTCPProbe::Bytes>();}
    else {
        using T=LocalTCPProbe;request.assign(197,0);
        const T::Bytes prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};std::copy(prefix.begin(),prefix.end(),request.begin());
        request[22]=12;const std::string phone="03123456789";std::copy(phone.begin(),phone.end(),request.begin()+23);
        request[81]='T';request[135]=0x1e;request[138]=89;T::putLong(request,139,13);
        const T::Bytes raw{1,2,3,4,5,6,7,0,0,0,1,7,1};std::copy(raw.begin(),raw.end(),request.begin()+143);
        request[156]=0x0c;T::putLong(request,157,0x10003);request[172]=0x0e;request[173]=4;
        request[174]=0x1c;request[175]=16;request[188]=2;request[189]=1;
        request.insert(request.end(),{0x15,0,2,0,1,0,0,1,0x14});request.insert(request.end(),276,0);
        request.insert(request.end(),{2,1,0,0,1,0x44});request.insert(request.end(),324,0);
        request.insert(request.end(),{0x16,0,0,0x1d,0,0,0x20,0,0,0,84});
        T::Bytes result(84,0);T::putLong(result,0,0x10003);request.insert(request.end(),result.begin(),result.end());
        request.insert(request.end(),4,0);request.insert(request.end(),{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
    }
    check(LocalTCPProbe::completeMailProbeRequest(request));
    const auto at=xband::registrationOffset(request,135),length=xband::registrationCardLength(request);
    const auto card=media_card::registrationCardReport(std::span<const uint8_t>(request).subspan(at,8+length));
    check(card.value==89&&length==13);
    const auto root=std::filesystem::temp_directory_path()/("xband-mail-credit-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    unsigned cases=0;
    for(unsigned mode=0;mode<9;++mode){
        auto settings=std::make_shared<ServiceCreditSettings>(root/(std::to_string(mode)+"-settings.json"));
        const unsigned amount=mode==8?3:1;
        settings->save(mode==2?0:mode==3?32767:amount,3,mode!=1);
        const auto ledgerPath=root/(std::to_string(mode)+"-ledger.json");
        auto ledger=std::make_shared<DeferredCreditSettlement>(ledgerPath);
        auto history=std::make_shared<ActivityHistory>(root/(std::to_string(mode)+"-history"));
        PB3Service service(0,std::make_shared<PB3Observation>(),std::make_shared<xband::LocalMatchRoles>(),false,false);
        service.setServiceCredits(settings,ledger);service.setActivityHistory(history);
        auto& probe=service.testTCP();probe.captured=request;probe.state=LocalTCPProbe::State::Established;probe.serviceWindow=65535;
        // This fixture isolates billing, without production display globals.
        probe.gameIntroTitleReply={};probe.gameMatchAwardReply={};probe.regionTownReply={};probe.usageAreaPreferenceReply={};probe.usageAreaReply={};
        uint64_t clock=0;probe.cardDebitClock=[&clock]{return clock;};
        unsigned prepared=0;probe.prepareServiceReply=[&]{++prepared;return true;};
        if(mode==4){probe.captured[xband::registrationAfterCardOffset(request,160)]=3;}
        if(mode==5){probe.cardDebit.arm(1,60000,true);} // Explicit diagnostic wins, never two49s.
        const auto packet=probe.pollServiceReply();
        if(mode==1||mode==2||mode==4){check(!packet.empty()&&ledger->snapshot().empty()&&prepared==1);++cases;continue;}
        if(mode==3){check(!packet.empty()&&packet[40]==0x22&&packet.back()==2&&
            probe.creditDenialSent&&ledger->snapshot().empty()&&prepared==0);++cases;continue;}
        check(!packet.empty()&&probe.cardDebit.state==media_card::ServiceDebitExchange::State::Waiting&&prepared==0);
        check(probe.pollServiceReply().empty()); // No retransmission.
        if(mode==5){check(ledger->snapshot().empty());++cases;continue;}
        check(ledger->snapshot().begin().value().at("state")=="issued");
        if(mode==7){clock=60000;check(probe.pollServiceReply().empty());}
        else {
            auto updated=*card.raw;updated[12]=0;
            if(mode==8){updated[11]=3;updated[12]=63;}
            LocalTCPProbe::Bytes reply{0x1e,0,0,uint8_t(mode==6?89:89-amount),0,0,0,13};
            reply.insert(reply.end(),updated.begin(),updated.end());reply.insert(reply.end(),{0,0,0,uint8_t(amount)});
            probe.cardDebit.receive(reply,clock);
            const auto continuation=probe.pollServiceReply();
            check((mode==0||mode==8)==!continuation.empty());
        }
        const auto expected=mode==0||mode==8?"confirmed":"uncertain";
        check(ledger->snapshot().begin().value().at("state")==expected);
        if(mode==0||mode==8){check(probe.cardDebitReleased&&prepared==1);check(probe.pollServiceReply().empty());}
        service.reset();
        auto reopened=std::make_shared<DeferredCreditSettlement>(ledgerPath);
        check(reopened->snapshot().begin().value().at("state")==expected);
        if(mode!=0&&mode!=8){bool rejected=false;try{auto next=std::make_shared<ServiceMailCredit>(reopened,xband::registrationPhone(request),"another",1,card);}catch(...){rejected=true;}check(rejected);}
        const auto savedHistory=history->page(0);
        for(const auto& row:savedHistory.at("rows")){
            check(!row.contains("credit_trial_session"));
            if(row.value("event",std::string{})=="credit_service_confirmed")check(row.at("remaining_credits")==89-amount);
        }
        ++cases;
    }
    for(unsigned balance:{0u,1u,2u}){
        auto history=std::make_shared<ActivityHistory>(root/("partial-"+std::to_string(balance)));
        auto trials=std::make_shared<CardDebitTrials>();
        PB3Service service(0,std::make_shared<PB3Observation>(),std::make_shared<xband::LocalMatchRoles>(),false,false);
        service.setActivityHistory(history);service.setCardDebitTrials(trials);
        auto& probe=service.testTCP();probe.armCardDebitTrial(3,60000);
        probe.captured=request;probe.captured[at+2]=0;probe.captured[at+3]=uint8_t(balance);
        std::fill(probe.captured.begin()+at+16,probe.captured.begin()+at+21,0);
        probe.captured[at+20]=uint8_t((1u<<balance)-1);
        probe.state=LocalTCPProbe::State::Established;probe.serviceWindow=65535;
        probe.cardDebitClock=[](){return uint64_t(0);};
        unsigned prepared=0,ordinary=0;
        probe.prepareServiceReply=[&]{++prepared;return true;};probe.onServiceReplySent=[&]{++ordinary;};
        const auto sent=probe.pollServiceReply();check(sent.size()==62&&sent[40]==0x49);
        auto raw=*card.raw;std::fill(raw.begin()+8,raw.end(),0);
        LocalTCPProbe::Bytes reply{0x1e,0,0,0,0,0,0,13};
        reply.insert(reply.end(),raw.begin(),raw.end());reply.insert(reply.end(),{0,0,0,uint8_t(balance)});
        probe.cardDebit.receive(reply,0);
        const auto denial=probe.pollServiceReply();service.publishCardTrial();
        check(!denial.empty()&&denial[40]==0x22&&denial.back()==2&&probe.creditDenialSent&&
            !probe.cardDebitReleased&&probe.cardDebit.result==balance&&prepared==0&&ordinary==0);
        const auto textLength=LocalTCPProbe::longword(denial,49);
        check(denial.size()==40+13+textLength+1&&denial[53+textLength-1]==0);
        check(trials->snapshot(0).phase=="Insufficient"&&!trials->snapshot(0).continuationVerified);
        bool blocked=false;try{trials->continueTrial(0);}catch(...){blocked=true;}check(blocked);
        check(probe.pollServiceReply().empty());
        const auto rows=history->page(0).at("rows");
        bool resultFound=false,denialFound=false;
        for(const auto& row:rows){
            if(row.value("event",std::string{})=="credit_trial_result"){
                check(row.at("requested_credits")==3&&row.at("consumed_credits")==balance&&
                    row.at("credits_before")==balance&&row.at("remaining_credits")==0&&row.at("credit_trial_shortfall")==true);
                resultFound=true;
            }
            denialFound|=row.value("event",std::string{})=="credit_trial_insufficient";
        }
        check(resultFound&&denialFound);service.reset();
        check(probe.cardDebit.state==media_card::ServiceDebitExchange::State::Disabled&&!probe.creditDenialSent);
    }
    std::cout<<"PASS 3 integrated exhausted-card rejections: request3 with0/1/2,22/02 only, no mail/matching, retained actual/balance history, no manual success or reset replay\n";
    std::cout<<"PASS "<<cases<<" integrated automatic mail cases: confirmed continuation, off/zero, insufficient notice without debit, match isolation, manual priority, mismatch/timeout restart block; synthetic/offline only\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
