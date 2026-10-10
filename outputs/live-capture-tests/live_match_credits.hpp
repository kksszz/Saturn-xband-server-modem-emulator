#pragma once
#include "deferred_credit_settlement.hpp"
#include "credit_result_policy.hpp"
#include "local_tcp_probe.hpp"
#include <array>
#include <mutex>

namespace diagnostic {
// Local server correlation, NOT a ROM transaction ID. No history scan/import.
// One automatically payable episode per endpoint/account; deferred evidence
// remains archived without blocking new episodes. Baselines frozen at carrier
// start; each endpoint settles from its own fresh report, independently of
// peer access. Reciprocal comparison is audit-only, never a debit barrier.
class LiveMatchCredits {
    using J=nlohmann::json;
    std::filesystem::path path;mutable std::mutex mutex;
    J file={{"schema",1},{"kind","live-match-credit-episodes"},{"next",1},{"episodes",J::object()}};
    J saved=nullptr;
    void commit(J next){
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        auto lockPath=path;lockPath+=L".lock";
        auto handle=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Match credit store locked");
        struct Close{HANDLE value;~Close(){CloseHandle(value);}} close{handle};
        J current=nullptr;if(std::filesystem::exists(path)){std::ifstream in(path,std::ios::binary);current=J::parse(in);}
        if(current!=saved)throw std::runtime_error("Stale match credit store; no merge or charge");
        auto temp=path;temp+=L".tmp";if(std::filesystem::exists(temp))throw std::runtime_error("Unresolved match credit commit");
        {std::ofstream out(temp,std::ios::binary);out<<next.dump(2);out.flush();if(!out)throw std::runtime_error("Match credit write failed");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Match credit commit failed");
        saved=next;file=std::move(next);
    }
    static bool pending(const J& row,unsigned side){const auto& p=row.at("participants").at(side);return !p.value("deferred",false)&&!p.value("settled",p.at("paid").get<bool>());}
    static bool finiteCard(const J& card){
        if(!card.is_object()||!card.contains("raw")||!card.at("raw").is_array()||card.at("raw").size()!=13||
           !card.contains("value")||!card.at("value").is_number_integer())return false;
        const auto value=card.at("value").get<int64_t>();if(value<0||value>32767)return false;
        int64_t bits=0;
        for(unsigned i=0;i<13;++i){const auto& b=card.at("raw").at(i);
            if(!b.is_number_integer()||b.get<int64_t>()<0||b.get<int64_t>()>255)return false;
            if(i>=8){unsigned count=0;for(unsigned n=0;n<8;++n)count+=(b.get<unsigned>()>>n)&1u;bits=bits*8+count;}}
        return value==bits;
    }
    static int64_t error(const J& report){const auto& b=report.at("raw");uint32_t bits=0;for(unsigned i=8;i<12;++i)bits=(bits<<8)|b.at(i).get<uint8_t>();return bits&0x80000000u?int64_t(bits)-0x100000000LL:int64_t(bits);}
    static uint32_t word(const J& report,unsigned at){uint32_t n=0;for(unsigned i=0;i<4;++i)n=(n<<8)|report.at("raw").at(at+i).get<uint8_t>();return n;}
    static void resolve(J& row){
        if(row.at("phase")=="open"||row.at("phase")=="blocked")return;
        if(!row.contains("fees"))row["fees"]=J::array({nullptr,nullptr});
        if(!row.contains("outcomes"))row["outcomes"]=J::array({nullptr,nullptr});
        for(unsigned side=0;side<2;++side){
            const auto& report=row.at("reports").at(side);
            // Preserve already frozen legacy fees/claims; no retrospective
            // repricing when a peer reports or an old ledger is reopened.
            if(row.at("participants").at(side).value("deferred",false)||report.is_null()||!row.at("fees").at(side).is_null()||word(report,4)!=row.at("game"))continue;
            const auto e=error(report);
            if(report.at("opcode")==0x20&&e==0&&
               uint64_t(word(report,12))+word(report,16)+word(report,20)+word(report,24)>0){
                row["fees"][side]=row.at("normal_units");row["outcomes"][side]="normal-local-result";
            }else if(creditReset(report.at("opcode").get<uint8_t>(),e)){
                row["fees"][side]=1;row["outcomes"][side]="local-or-peer-reset";
            }
        }
        row["phase"]=!row.at("fees").at(0).is_null()&&!row.at("fees").at(1).is_null()?"ready":"ended";
        row["comparison"]="awaiting-peer-report";
        if(row.at("reports").at(0).is_null()||row.at("reports").at(1).is_null())return;
        const auto& a=row.at("reports").at(0);const auto& b=row.at("reports").at(1);
        row["comparison"]="unresolved-or-conflicting-reports";
        if(word(a,4)!=row.at("game")||word(b,4)!=row.at("game"))return;
        const auto ea=error(a),eb=error(b);
        if(a.at("opcode")==0x20&&b.at("opcode")==0x20&&ea==0&&eb==0&&
            uint64_t(word(a,12))+word(a,16)+word(a,20)+word(a,24)>0&&
            word(a,12)==word(b,20)&&word(a,16)==word(b,24)&&word(a,20)==word(b,12)&&word(a,24)==word(b,16)){
            row["comparison"]="normal-reciprocal-results";
        }else if(a.at("opcode")==0x23&&b.at("opcode")==0x23&&
            ((ea==-608&&eb==-609)||(ea==-609&&eb==-608))){
            row["comparison"]="local-and-peer-reset";
        }
        // Unknown errors and sentinel recovery
        // remain unresolved for THAT endpoint. Never reinterpret the peer's
        // report or change a previously frozen claim/confirmed card receipt.
    }
public:
    // Explicit local policy frozen by the carrier hook. Importing legacy
    // settings does not enable it; new matches require an operator opt-in.
    struct Policy{bool enabled=false;unsigned normal=3;int resetScope=1,includeMail=1;};
    struct Plan{enum class State{None,Waiting,Ready,Blocked,Deferred};State state=State::None;std::string episode,debitKey;unsigned amount=0;bool mailDenied=false;};
    explicit LiveMatchCredits(std::filesystem::path value):path(std::move(value)){
        auto temp=path;temp+=L".tmp";if(std::filesystem::exists(temp))throw std::runtime_error("Interrupted match credit commit");
        if(!std::filesystem::exists(path))return;std::ifstream in(path,std::ios::binary);file=J::parse(in);saved=file;
        if(file.at("schema")!=1||file.at("kind")!="live-match-credit-episodes"||!file.at("episodes").is_object()||!file.at("next").is_number_unsigned())
            throw std::runtime_error("Invalid match credit store");
        for(const auto& row:file.at("episodes")){
            const auto phase=row.at("phase").get<std::string>();
            if(!row.at("participants").is_array()||row.at("participants").size()!=2||
                !row.at("reports").is_array()||row.at("reports").size()!=2||row.at("normal_units").get<unsigned>()>32767||
                (phase!="open"&&phase!="ended"&&phase!="ready"&&phase!="blocked")||
                row.at("reset_scope")!=1||row.at("include_mail")!=1)
                throw std::runtime_error("Invalid saved match credit episode");
            if(row.contains("fees")){
                if(!row.at("fees").is_array()||row.at("fees").size()!=2)throw std::runtime_error("Invalid saved match fees");
                for(const auto& fee:row.at("fees"))if(!fee.is_null()&&
                    (!fee.is_number_integer()||fee.get<int64_t>()<0||fee.get<int64_t>()>32767))
                    throw std::runtime_error("Invalid saved match fee");
            }
            if(row.contains("outcomes")){
                if(!row.at("outcomes").is_array()||row.at("outcomes").size()!=2)throw std::runtime_error("Invalid saved local outcomes");
                for(const auto& outcome:row.at("outcomes"))if(!outcome.is_null()&&!outcome.is_string())
                    throw std::runtime_error("Invalid saved local outcome");
            }
            for(const auto& p:row.at("participants")){
                if(p.at("phone").get<std::string>().empty()||p.at("profile").get<unsigned>()>3||
                    !p.at("paid").is_boolean()||!p.at("baseline").is_array()||
                    (!p.at("baseline").empty()&&p.at("baseline").size()!=84)||!finiteCard(p.at("card"))||
                    (p.contains("settled")&&!p.at("settled").is_boolean())||
                    (p.contains("report_conflict")&&!p.at("report_conflict").is_boolean())||
                    (p.contains("deferred")&&!p.at("deferred").is_boolean()))
                    throw std::runtime_error("Invalid saved match participant");
                if(p.value("deferred",false)&&(!p.at("claim").is_null()||p.value("settled",false)||p.at("paid").get<bool>()))
                    throw std::runtime_error("Deferred match cannot have a debit claim or receipt");
                if(!p.at("claim").is_null()){
                    const auto& claim=p.at("claim");
                    if(claim.at("amount").get<unsigned>()>32767||!claim.at("mail_denied").is_boolean()||
                       !finiteCard(claim.at("card")))throw std::runtime_error("Invalid saved match claim");
                }
            }
            for(const auto& report:row.at("reports"))if(!report.is_null()){
                if((report.at("opcode")!=0x20&&report.at("opcode")!=0x23)||
                   !report.at("raw").is_array()||report.at("raw").size()!=84||word(report,0)!=84)
                    throw std::runtime_error("Invalid saved match report");
                for(const auto& b:report.at("raw"))if(!b.is_number_integer()||b.get<int64_t>()<0||b.get<int64_t>()>255)
                    throw std::runtime_error("Invalid saved match report byte");
            }
        }
    }
    J snapshot()const{std::lock_guard lock(mutex);return file.at("episodes");}
    std::string begin(uint64_t generation,const std::array<J,2>& contexts,const Policy& policy){
        if(!policy.enabled)return {};
        if(policy.resetScope!=1||policy.includeMail!=1||policy.normal>32767)throw std::runtime_error("Both-side reset1/additive mail required");
        std::lock_guard lock(mutex);J participants=J::array();
        const auto game=contexts[0].at("game").get<uint32_t>();
        for(unsigned side=0;side<2;++side){
            const auto& c=contexts[side];
            if(c.at("game")!=game||c.at("phone").get<std::string>().empty()||c.at("profile").get<unsigned>()>3||!c.contains("credit_baseline"))
                throw std::runtime_error("Two complete registered endpoints required for credit episode");
            if(!c.at("credit_baseline").is_array()||
               (!c.at("credit_baseline").empty()&&c.at("credit_baseline").size()!=84))
                throw std::runtime_error("Invalid frozen result baseline");
            const auto card=c.at("credit_card");
            // Live ROM testing allowed a match with only2 units. Do not invent
            // a minimum3 admission rule; later49 proves actual exhaustion.
            if(!finiteCard(card))throw std::runtime_error("Consistent finite match card required");
            for(const auto& previous:file.at("episodes"))for(unsigned endpoint=0;endpoint<2;++endpoint){
                const auto& participant=previous.at("participants").at(endpoint);
                const auto prior=participant.at("card").at("raw").get<std::array<uint8_t,13>>();
                const auto current=card.at("raw").get<std::array<uint8_t,13>>();
                if(pending(previous,endpoint)&&(participant.at("phone")==c.at("phone")||
                   std::equal(prior.begin(),prior.begin()+8,current.begin())))
                    throw std::runtime_error("Unsettled owner/card exists; cannot start a second episode");
            }
            if(side&&contexts[0].at("phone")==c.at("phone"))throw std::runtime_error("Distinct participants required");
            if(side){const auto prior=contexts[0].at("credit_card").at("raw").get<std::array<uint8_t,13>>();
                const auto current=card.at("raw").get<std::array<uint8_t,13>>();
                if(std::equal(prior.begin(),prior.begin()+8,current.begin()))throw std::runtime_error("Distinct cards required");}
            participants.push_back({{"phone",c.at("phone")},{"profile",c.at("profile")},{"baseline",c.at("credit_baseline")},
                {"card",card},{"paid",false},{"settled",false},{"settlement_state","unpaid"},{"claim",nullptr}});
        }
        auto next=file;const auto id=next.at("next").get<uint64_t>();if(id==UINT64_MAX)throw std::runtime_error("Match episode counter exhausted");
        const auto key="match/"+std::to_string(id);if(next.at("episodes").contains(key))throw std::runtime_error("Match episode key collision");next["next"]=id+1;
        next["episodes"][key]={{"generation",generation},{"game",game},{"phase","open"},{"normal_units",policy.normal},
            {"reset_scope",policy.resetScope},{"include_mail",policy.includeMail},{"participants",participants},{"reports",J::array({nullptr,nullptr})}};
        commit(std::move(next));return key;
    }
    void end(const std::string& key){if(key.empty())return;std::lock_guard lock(mutex);if(file.at("episodes").at(key).at("phase")!="open")return;
        auto next=file;next["episodes"][key]["phase"]="ended";resolve(next["episodes"][key]);commit(std::move(next));}
    Plan observeAndPlan(unsigned side,const LocalTCPProbe::Bytes& request,unsigned mailUnits,
        bool mailEnabled,DeferredCreditSettlement& debits){
        if(side>1)throw std::runtime_error("Invalid reporting endpoint");
        if(mailUnits>32767||!LocalTCPProbe::completeMailProbeRequest(request,true))return {Plan::State::Blocked};
        const auto phone=xband::registrationPhone(request);uint8_t opcode=0;const auto raw=LocalTCPProbe::observedGameResult(request,true,&opcode);
        const auto cardAt=xband::registrationOffset(request,135),length=xband::registrationCardLength(request);
        const auto card=media_card::registrationCardReport(std::span<const uint8_t>(request).subspan(cardAt,8+length));
        std::lock_guard lock(mutex);std::string key;
        for(auto it=file.at("episodes").begin();it!=file.at("episodes").end();++it){
            for(unsigned endpoint=0;endpoint<2;++endpoint){
                if(!pending(it.value(),endpoint))continue;
                const auto& prior=it.value().at("participants").at(endpoint);
                const auto identity=prior.at("card").at("raw").get<std::array<uint8_t,13>>();
                const bool samePhone=prior.at("phone")==phone;
                const bool sameCard=card.raw&&std::equal(identity.begin(),identity.begin()+8,card.raw->begin());
                if(!samePhone&&!sameCard)continue;
                // Moving a pending owner/card to the other endpoint must not
                // bypass match-first settlement and silently charge mail.
                if(endpoint!=side||!samePhone||!key.empty())return {Plan::State::Blocked,it.key()};
                key=it.key();
            }
        }
        if(key.empty())return {};
        auto next=file;auto& row=next["episodes"][key];auto& participant=row["participants"][side];
        const auto debitKey=key+"/side:"+std::to_string(side);
        const auto starting=participant.at("card").at("raw").get<std::array<uint8_t,13>>();
        if(!card.raw||!finiteCard(J{{"value",card.value},{"raw",*card.raw}})||
           !std::equal(starting.begin(),starting.begin()+8,card.raw->begin()))return {Plan::State::Blocked,key,debitKey};
        const auto ledger=debits.snapshot();
        // Only an ended, unclaimed episode may leave automatic settlement.
        // Keep its evidence permanently; never attach a later result to it.
        // Issued/uncertain charges and identity/storage faults stay fail-closed.
        const auto defer=[&](const char* reason)->Plan{
            if(row.at("phase")=="open"||row.at("phase")=="blocked"||
               !participant.at("claim").is_null()||ledger.contains(debitKey))return {Plan::State::Blocked,key,debitKey};
            participant["deferred"]=true;participant["settlement_state"]="deferred-review";
            participant["deferred_reason"]=reason;participant["deferred_observed_raw"]=raw;
            commit(next);return {Plan::State::Deferred,key,debitKey};
        };
        if(request.at(xband::registrationOffset(request,43))!=participant.at("profile")||
           LocalTCPProbe::longword(request,xband::registrationAfterCardOffset(request,144))!=row.at("game"))
            return defer("registered-context-changed");
        if(!participant.at("claim").is_null()&&ledger.contains(debitKey)&&
           (ledger.at(debitKey).at("state")=="confirmed"||ledger.at(debitKey).at("state")=="exhausted")){
            const auto& claim=participant.at("claim");
            if(ledger.at(debitKey).at("amount")!=claim.at("amount")||ledger.at(debitKey).at("owner")!=phone||
               ledger.at(debitKey).at("initial_card")!=claim.at("card"))return {Plan::State::Blocked,key,debitKey};
            const auto& updated=ledger.at(debitKey).at("updated_card");
            if(updated.at("value")!=card.value||updated.at("raw")!=J(*card.raw))return {Plan::State::Blocked,key,debitKey};
            // A native-confirmed debit cannot be sent again after a crash
            // between the debit ledger and the match ledger commits. This
            // recovery synchronizes the receipt; the caller handles any new
            // uncharged outgoing letters separately, never repeats this49.
            const bool full=ledger.at(debitKey).at("state")=="confirmed";
            participant["paid"]=full;participant["settled"]=true;
            participant["settlement_state"]=full?"confirmed":"exhausted";
            participant["consumed"]=ledger.at(debitKey).at("consumed");
            const bool denied=claim.at("mail_denied").get<bool>()||!full;
            commit(std::move(next));return {Plan::State::Ready,key,debitKey,0,denied};
        }
        if(row.at("phase")=="blocked")return {Plan::State::Blocked,key,debitKey};
        if(row.at("phase")=="open")return {Plan::State::Waiting,key,debitKey};
        if(raw.empty()||J(raw)==participant.at("baseline"))return defer("fresh-result-missing");
        const J report{{"opcode",opcode},{"raw",raw}};
        if(participant.value("report_conflict",false))return {Plan::State::Blocked,key,debitKey};
        if(row["reports"][side].is_null()){row["reports"][side]=report;resolve(row);commit(next);}
        else if(row.at("reports").at(side)!=report){participant["report_conflict"]=true;commit(next);return {Plan::State::Blocked,key,debitKey};}
        else{resolve(row);if(next!=file)commit(next);}
        if(!row.contains("fees")||row.at("fees").at(side).is_null())return defer("result-unclassified");
        if(!participant.at("claim").is_null())return {Plan::State::Ready,key,debitKey,participant.at("claim").at("amount").get<unsigned>(),participant.at("claim").at("mail_denied").get<bool>()};
        const unsigned matchUnits=row.at("fees").at(side).get<unsigned>();
        // A request may exceed the finite balance. Keep the full match request
        // and let the verified native reply record the partial actual debit.
        // Caller supplies the actual uncharged outgoing-letter total, not
        // a connection fee. Letters can accompany any supported service code.
        const unsigned mail=mailEnabled?mailUnits:0;
        const bool denied=uint64_t(card.value)<uint64_t(matchUnits)+mail;
        const unsigned amount=matchUnits+(denied?0:mail);
        if(amount>32767)return {Plan::State::Blocked,key,debitKey};
        participant["claim"]={{"amount",amount},{"match_units",matchUnits},{"mail_units",denied?0:mail},{"mail_denied",denied},
            {"card",{{"value",card.value},{"raw",*card.raw}}}};
        if(amount==0){participant["paid"]=participant["settled"]=true;participant["settlement_state"]="confirmed";}
        commit(std::move(next));return {Plan::State::Ready,key,debitKey,amount,denied};
    }
    void recordReceipt(const std::string& key,unsigned side,DeferredCreditSettlement& debits){
        if(side>1||key.empty())throw std::runtime_error("Invalid match receipt binding");
        std::lock_guard lock(mutex);auto next=file;auto& p=next["episodes"].at(key)["participants"].at(side);
        const auto receipt=debits.snapshot().at(key+"/side:"+std::to_string(side));
        const bool full=receipt.at("state")=="confirmed",exhausted=receipt.at("state")=="exhausted";
        if(p.at("claim").is_null()||(!full&&!exhausted)||receipt.at("owner")!=p.at("phone")||
           receipt.at("amount")!=p.at("claim").at("amount")||receipt.at("initial_card")!=p.at("claim").at("card")||
           !finiteCard(receipt.at("updated_card")))
            throw std::runtime_error("Native durable proof required before receipt mark");
        if(p.value("settled",false))return;p["paid"]=full;p["settled"]=true;
        p["settlement_state"]=full?"confirmed":"exhausted";p["consumed"]=receipt.at("consumed");
        commit(std::move(next));
    }
};
}
