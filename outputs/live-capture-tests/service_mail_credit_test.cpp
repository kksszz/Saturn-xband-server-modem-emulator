#define PB3_SERVICE_SERVER_TEST 1
#include "pb3_service_server.cpp"
#include <source_location>
namespace {
using J=nlohmann::json;using B=LocalTCPProbe::Bytes;using namespace diagnostic;
void check(bool ok,const std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("Per-letter mail assertion line "+std::to_string(at.line()));
}
media_card::CardReport card(unsigned balance){
    std::array<uint8_t,13> raw{1,2,3,4,5,6,7,8};unsigned n=balance;
    for(int i=12;i>=8;--i){raw[i]=uint8_t((1u<<(n&7))-1);n>>=3;}
    return {int32_t(balance),raw};
}
B reply(unsigned amount,unsigned balance){
    auto c=card(balance-amount);B out{0x1e,0,uint8_t((balance-amount)>>8),uint8_t(balance-amount),0,0,0,13};
    out.insert(out.end(),c.raw->begin(),c.raw->end());out.insert(out.end(),{0,0,uint8_t(amount>>8),uint8_t(amount)});return out;
}
B request(unsigned balance,const std::vector<std::pair<unsigned,std::string>>& letters={},unsigned code=4){
    B b(197,0);const B prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};std::copy(prefix.begin(),prefix.end(),b.begin());
    b[22]=12;const std::string phone="03123456789";std::copy(phone.begin(),phone.end(),b.begin()+23);
    b[81]='T';b[121]=6;b[128]=0x0b;b[135]=0x1e;b[138]=uint8_t(balance);b[137]=uint8_t(balance>>8);
    LocalTCPProbe::putLong(b,139,13);const auto c=card(balance);std::copy(c.raw->begin(),c.raw->end(),b.begin()+143);
    b[156]=0x0c;LocalTCPProbe::putLong(b,157,0x10003);b[172]=0x0e;b[173]=uint8_t(code);b[174]=0x1c;b[175]=16;b[188]=2;b[189]=1;
    b.insert(b.end(),{0x15,0,2,0,1,0,0,1,0x14});b.insert(b.end(),276,0);
    b.insert(b.end(),{2,1,0,0,1,0x44});b.insert(b.end(),324,0);
    b.insert(b.end(),{0x16,0,0,0x1d,0,uint8_t(letters.size())});
    for(const auto& [slot,body]:letters){
        b.push_back(uint8_t(slot));b.insert(b.end(),9,0xff);
        for(const auto& field:{std::string("DEST"),std::string("TITLE"),body}){
            b.push_back(0);b.push_back(uint8_t(field.size()+1));b.insert(b.end(),field.begin(),field.end());b.push_back(0);}
        b.insert(b.end(),4,0);
    }
    b.insert(b.end(),{0x20,0,0,0,84});b.insert(b.end(),84,0);b.insert(b.end(),4,0);
    b.insert(b.end(),{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
    check(LocalTCPProbe::completeMailProbeRequest(b));check(outgoingMailBatch(b).size()==letters.size());return b;
}
struct Fixture{
    std::filesystem::path root;
    std::shared_ptr<ServiceCreditSettings> settings;
    std::shared_ptr<DeferredCreditSettlement> ledger;
    std::shared_ptr<LocalMailJournal> journal;
    Fixture(std::filesystem::path p,unsigned unit=1,bool enabled=true):root(std::move(p)){
        settings=std::make_shared<ServiceCreditSettings>(root/"settings.json");settings->save(unit,3,enabled);
        reopen();
    }
    void reopen(){ledger=std::make_shared<DeferredCreditSettlement>(root/"debits.json");journal=std::make_shared<LocalMailJournal>(root/"journal");}
    std::unique_ptr<PB3Service> service(B wire){
        auto s=std::make_unique<PB3Service>(0,std::make_shared<PB3Observation>(),std::make_shared<xband::LocalMatchRoles>(),false,false);
        s->setServiceCredits(settings,ledger);s->setLocalMailJournal(journal);
        auto& p=s->testTCP();p.captured=std::move(wire);p.state=LocalTCPProbe::State::Established;p.serviceWindow=65535;
        p.gameIntroTitleReply={};p.gameMatchAwardReply={};p.regionTownReply={};p.usageAreaPreferenceReply={};p.usageAreaReply={};
        p.cardDebitClock=[](){return uint64_t(0);};return s;
    }
};
void pay(PB3Service& service,unsigned balance,unsigned amount){
    auto& p=service.testTCP();p.serviceWindow=21;check(p.pollServiceReply().empty());
    p.serviceWindow=65535;const auto packet=p.pollServiceReply();
    check(packet.size()==62&&packet[40]==0x49&&LocalTCPProbe::longword(packet,41)==amount);
    check(p.pollServiceReply().empty());p.cardDebit.receive(reply(amount,balance),0);
    const auto continuation=p.pollServiceReply();check(!continuation.empty()&&p.cardDebitReleased&&!p.creditDenialSent);
    check(p.pollServiceReply().empty());
}
}
int main(){try{
    // Fixed instruction body, bounded IDs, original CRC polynomial and02 end.
    for(const auto [id,crc]:{std::pair<uint16_t,uint16_t>{0x42,42600},{0x75,58973},{0x107,9857}}){
        const auto packet=originalCardWarningReply(id);
        check(packet.size()==53&&packet[0]==0x0a&&packet[1]==0&&packet.back()==2);
        check((uint16_t(packet[2])<<8|packet[3])==crc);
        check(LocalTCPProbe::longword(packet,4)==44&&LocalTCPProbe::longword(packet,8)==36&&LocalTCPProbe::longword(packet,12)==0);
        check(packet[48]==uint8_t(id>>8)&&packet[49]==uint8_t(id));
    }
    bool invalid=false;try{(void)originalCardWarningReply(0x43);}catch(const std::runtime_error&){invalid=true;}check(invalid);
    const auto root=std::filesystem::temp_directory_path()/("xband-per-letter-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    // Outbox submission on automatic matchmaking is not a free send.
    {
        Fixture f(root/"matchmaking-send");auto s=f.service(request(10,{{0,"on-matchmaking"}},3));auto& p=s->testTCP();
        const auto debit=p.pollServiceReply();check(!debit.empty()&&debit[40]==0x49&&LocalTCPProbe::longword(debit,41)==1);
        p.cardDebit.receive(reply(1,10),0);(void)p.pollServiceReply();
        check(p.cardDebitReleased&&f.journal->summary().at("mails")==1);
        s.reset();f.reopen();auto again=f.service(request(9,{{0,"on-matchmaking"}}));
        check(again->testTCP().pollServiceReply()[40]!=0x49&&f.journal->summary().at("mails")==1);
    }
    for(unsigned balance:{0u,1u,10u}){
        Fixture f(root/("receive-"+std::to_string(balance)));auto s=f.service(request(balance));
        const auto packet=s->testTCP().pollServiceReply();check(!packet.empty()&&packet[40]!=0x49&&!s->testTCP().creditDenialSent&&f.ledger->snapshot().empty());
        check(f.journal->summary().at("mails")==0);
    }
    for(unsigned count:{1u,2u,3u,4u}){
        Fixture f(root/("letters-"+std::to_string(count)));std::vector<std::pair<unsigned,std::string>> letters;
        for(unsigned i=0;i<count;++i)letters.push_back({i,"body"+std::to_string(i)});
        auto s=f.service(request(10,letters));pay(*s,10,count);
        check(f.ledger->snapshot().size()==1&&f.ledger->snapshot().begin().value().at("consumed")==count&&f.journal->summary().at("mails")==count);
        s.reset();f.reopen();std::reverse(letters.begin(),letters.end());
        auto again=f.service(request(10-count,letters));const auto packet=again->testTCP().pollServiceReply();
        check(!packet.empty()&&packet[40]!=0x49&&!again->testTCP().creditDenialSent&&f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==count);
        // An observed empty outbox makes a later identical intentional send new.
        auto empty=f.service(request(10-count));check(!empty->testTCP().pollServiceReply().empty());
        auto fresh=f.service(request(10-count,letters));pay(*fresh,10-count,count);
        check(f.ledger->snapshot().size()==2&&f.journal->summary().at("mails")==2*count);
    }
    {
        Fixture f(root/"equal-letters");auto s=f.service(request(10,{{0,"same"},{0,"same"}}));pay(*s,10,2);
        check(f.journal->summary().at("mails")==2); // Never collapse equal letters inside one batch.
    }
    {
        Fixture f(root/"new-ledger-existing-journal");
        auto first=f.service(request(10,{{0,"old-ledger-letter"}}));pay(*first,10,1);
        const auto oldID=f.ledger->snapshot().begin().key();first.reset();
        // Reuse a populated mail journal with a different, new debit ledger.
        f.ledger=std::make_shared<DeferredCreditSettlement>(f.root/"new-debits.json");
        auto next=f.service(request(32767,{{0,"new-ledger-letter"}}));pay(*next,32767,1);
        check(f.ledger->snapshot().begin().key()!=oldID&&f.journal->summary().at("mails")==2);
        next.reset();
        f.ledger=std::make_shared<DeferredCreditSettlement>(f.root/"new-debits.json");
        auto retry=f.service(request(32766,{{0,"new-ledger-letter"}}));
        check(!retry->testTCP().pollServiceReply().empty()&&
            retry->testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Disabled&&
            f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==2);
    }
    for(unsigned balance:{0u,1u}){
        Fixture f(root/("insufficient-"+std::to_string(balance)));auto s=f.service(request(balance,{{0,"one"},{3,"two"}}));
        const auto packet=s->testTCP().pollServiceReply();check(!packet.empty()&&packet[40]==(balance==0?0x0a:0x22)&&s->testTCP().creditDenialSent&&f.ledger->snapshot().empty()&&f.journal->summary().at("mails")==0);
    }
    {
        Fixture f(root/"free-rate",0);auto s=f.service(request(0,{{0,"free"}}));check(!s->testTCP().pollServiceReply().empty());
        check(f.ledger->snapshot().empty()&&f.journal->summary().at("mails")==1);
    }
    {
        Fixture f(root/"disabled",1,false);auto s=f.service(request(0,{{0,"off"}}));check(!s->testTCP().pollServiceReply().empty()&&f.ledger->snapshot().empty());
    }
    for(bool timeout:{false,true}){
        Fixture f(root/(timeout?"timeout":"disconnect"));auto s=f.service(request(10,{{0,"pending"}}));
        check(s->testTCP().pollServiceReply()[40]==0x49);
        if(timeout){s->testTCP().cardDebitClock=[](){return uint64_t(60000);};check(s->testTCP().pollServiceReply().empty());}
        s.reset();f.reopen();auto retry=f.service(request(9,{{0,"pending"}}));const auto packet=retry->testTCP().pollServiceReply();
        check(!packet.empty()&&packet[40]==0x22&&f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==0);
        // Free receive access is not a debit retry and is allowed at zero.
        auto receive=f.service(request(0));check(!receive->testTCP().pollServiceReply().empty()&&!receive->testTCP().creditDenialSent);
    }
    {
        // The reported incident: previous uncertain outbox plus a currently
        // inserted zero card. Current reason wins without rewriting history.
        Fixture f(root/"current-reason");auto pending=f.service(request(10,{{0,"old-pending"}}));
        check(pending->testTCP().pollServiceReply()[40]==0x49);pending.reset();f.reopen();
        auto readLedger=[&]{J data;std::ifstream input(f.root/"debits.json");input>>data;return data;};
        const auto before=readLedger();
        for(unsigned state:{0u,1u,2u,3u}){
            auto current=f.service(request(state==0?0:10,{{0,"changed"}}));
            if(state)current->setMatchCardAdmission([state]{return state==1?xband::CardMatchAdmission::missing:state==3?xband::CardMatchAdmission::unreadable:xband::CardMatchAdmission::unavailable;});
            current->testTCP().serviceWindow=0;
            check(current->testTCP().pollServiceReply().empty()&&!current->testTCP().creditDenialSent);
            current->testTCP().serviceWindow=65535;
            check(current->testTCP().pollServiceReply()[40]==(state==2?0x22:0x0a)&&current->testTCP().creditDenialSent);
            const auto expected=state==2?creditNoticeReply(
                L"カードの状態を確認できません。カードとエミュレーターの状態を確認してください。今回はメール送受信・対戦接続は行いません。"):
                originalCardWarningReply(state==0?0x42:state==1?0x75:0x107);
            check(current->testTCP().creditDenialReply==expected&&readLedger()==before&&f.journal->summary().at("mails")==0);
            check(current->testTCP().pollServiceReply().empty()); // Exactly one refusal; never automatically reissue.
        }
        auto positive=f.service(request(10,{{0,"changed"}}));check(positive->testTCP().pollServiceReply()[40]==0x22);
        check(positive->testTCP().creditDenialReply==creditNoticeReply(L"前回のメール送信結果を確認できません。二重送信・二重消費を防ぐため、今回はメール送受信・対戦接続は行いません。サーバーの記録を確認してください。"));
        check(readLedger()==before); // Restoring credit must not hide/cancel an uncertain debit.
    }
    {
        Fixture f(root/"zero-no-reservation");auto s=f.service(request(0,{{0,"new"}}));
        check(s->testTCP().pollServiceReply()[40]==0x0a&&f.ledger->snapshot().empty());
        check(!std::filesystem::exists(f.root/"debits.json")); // No unresolved reservation left by a zero-card refusal.
    }
    {
        Fixture f(root/"paid-before-journal");auto s=f.service(request(10,{{0,"crash"}}));auto& p=s->testTCP();
        check(p.pollServiceReply()[40]==0x49);p.cardDebit.receive(reply(1,10),0);
        p.prepareServiceReply=[](){return false;};check(p.pollServiceReply().empty()&&p.cardDebitReleased);s.reset();f.reopen();
        auto retry=f.service(request(9,{{0,"crash"}}));const auto packet=retry->testTCP().pollServiceReply();
        check(!packet.empty()&&packet[40]!=0x49&&f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==1);
    }
    {
        Fixture f(root/"paid-before-journal-zero");auto s=f.service(request(1,{{0,"paid"}}));auto& p=s->testTCP();
        check(p.pollServiceReply()[40]==0x49);p.cardDebit.receive(reply(1,1),0);
        p.prepareServiceReply=[](){return false;};check(p.pollServiceReply().empty()&&p.cardDebitReleased);s.reset();f.reopen();
        auto retry=f.service(request(0,{{0,"paid"}}));const auto packet=retry->testTCP().pollServiceReply();
        check(!packet.empty()&&packet[40]!=0x49&&!retry->testTCP().creditDenialSent&&f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==1);
    }
    {
        // Actual incident: old batch reserved, but no debit episode or wire
        // issue. A disjoint new letter must not be blocked by that reservation.
        Fixture f(root/"unissued-disjoint");const auto old=request(10,{{0,"waiting"}});
        const auto account=mailAccountKey(old).first;
        const auto reserved=f.ledger->planMailSend(account,outgoingMailBatch(old),1);
        check(!reserved.batch.empty()&&reserved.units==1&&f.ledger->snapshot().empty());
        f.reopen();auto fresh=f.service(request(10,{{1,"second"}}));pay(*fresh,10,1);
        check(f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==1);
        fresh.reset();f.reopen();
        // The old reservation remains reachable across restart; no automatic
        // send occurred, and requesting it explicitly charges only its own1.
        check(f.ledger->planMailSend(account,outgoingMailBatch(old),1).batch==reserved.batch);
        auto earlier=f.service(request(9,{{0,"waiting"}}));pay(*earlier,9,1);
        check(f.ledger->snapshot().size()==2&&f.journal->summary().at("mails")==2);
        auto duplicate=f.service(request(8,{{1,"second"}}));
        check(duplicate->testTCP().pollServiceReply()[40]!=0x49&&!duplicate->testTCP().creditDenialSent);
        check(f.ledger->snapshot().size()==2&&f.journal->summary().at("mails")==2);
    }
    {
        Fixture f(root/"paid-disjoint-before-journal");auto old=f.service(request(10,{{0,"paid-old"}}));auto& p=old->testTCP();
        check(p.pollServiceReply()[40]==0x49);p.cardDebit.receive(reply(1,10),0);
        p.prepareServiceReply=[](){return false;};check(p.pollServiceReply().empty()&&p.cardDebitReleased);
        old.reset();f.reopen();auto fresh=f.service(request(9,{{1,"new-other"}}));pay(*fresh,9,1);
        auto restore=f.service(request(8,{{0,"paid-old"}}));
        check(restore->testTCP().pollServiceReply()[40]!=0x49&&!restore->testTCP().creditDenialSent);
        check(f.ledger->snapshot().size()==2&&f.journal->summary().at("mails")==2);
    }
    {
        Fixture f(root/"unissued-queue-growth");const auto old=request(10,{{0,"queued"}});
        const auto account=mailAccountKey(old).first;
        const auto first=f.ledger->planMailSend(account,outgoingMailBatch(old),1);
        f.reopen();auto combined=f.service(request(10,{{0,"queued"},{1,"next"}}));pay(*combined,10,2);
        check(f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==2);
        J saved;std::ifstream file(f.root/"debits.json");file>>saved;
        const auto& prior=saved.at("mail_sends").at("batches").at(first.batch);
        check(prior.at("closed")==true&&prior.at("episode").is_null()&&prior.at("records").size()==1&&prior.contains("superseded_by"));
        combined.reset();f.reopen();auto reordered=f.service(request(8,{{1,"next"},{0,"queued"}}));
        check(reordered->testTCP().pollServiceReply()[40]!=0x49&&!reordered->testTCP().creditDenialSent);
        auto partial=f.service(request(8,{{0,"queued"}}));check(partial->testTCP().pollServiceReply()[40]==0x22);
        check(f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==2);
    }
    {
        Fixture f(root/"overlap");auto s=f.service(request(10,{{0,"old"}}));pay(*s,10,1);
        auto overlap=f.service(request(9,{{0,"old"},{1,"new"}}));check(overlap->testTCP().pollServiceReply()[40]==0x22&&f.ledger->snapshot().size()==1&&f.journal->summary().at("mails")==1);
        auto disjoint=f.service(request(9,{{1,"new"}}));pay(*disjoint,9,1);check(f.ledger->snapshot().size()==2);
    }
    std::cout<<"PASS per-letter1..4, equal-letter multiplicity, all slots, free receive0, opt-in/zero, insufficient whole-batch refusal, reordered/restarted duplicate suppression, empty-outbox generation, no retry on uncertainty, paid-before-journal recovery, ambiguous overlap refusal\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
