#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#pragma comment(lib,"ole32.lib")
#include <nlohmann/json.hpp>
#include "service_card_debit_exchange.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <algorithm>
#include <vector>

namespace diagnostic {
// Explicit reviewed settlement only. No automatic server queue or startup replay.
// An episode key and connection key MUST come from verified external context;
// raw result bytes, a reset opcode, or a report fingerprint are not episode IDs.
class DeferredCreditSettlement {
    using J=nlohmann::json;
    std::filesystem::path path;
    mutable std::mutex mutex;
    J rows=J::object();
    J mailSends={{"next",1},{"accounts",J::object()},{"batches",J::object()}};
    J savedFile=nullptr;
    struct FileLock {
        HANDLE handle=INVALID_HANDLE_VALUE;
        explicit FileLock(const std::filesystem::path& p){
            handle=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Settlement ledger busy or lock unavailable");
        }
        ~FileLock(){CloseHandle(handle);}
        FileLock(const FileLock&)=delete;
    };
    static void key(const std::string& s){if(s.empty()||s.size()>256)throw std::invalid_argument("Invalid settlement context key");}
    static std::string newMailBatchKey(uint64_t sequence){
        // A copied journal can outlive/rejoin a newly created debit ledger.
        // Local sequence alone is not a globally unique send identity.
        GUID guid{};wchar_t text[40]{};
        if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,text,40))
            throw std::runtime_error("Cannot allocate unique mail-send identity");
        const std::wstring wide(text);
        return "mail-send/"+std::string(wide.begin(),wide.end())+"/"+std::to_string(sequence);
    }
    static J cardJSON(const media_card::CardReport& c){
        if(!c.raw||c.value<0||c.value>32767)throw std::invalid_argument("Finite inserted card report0..32767 required");
        return J{{"value",c.value},{"raw",*c.raw}};
    }
    static media_card::CardReport card(const J& j){
        return {j.at("value").get<int32_t>(),j.at("raw").get<std::array<uint8_t,13>>()};
    }
    void commit(const J& next,const J& nextMail){
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        auto lockPath=path;lockPath+=L".lock";FileLock fileLock(lockPath);
        auto temp=path;temp+=L".tmp";
        if(std::filesystem::exists(temp))throw std::runtime_error("Unresolved settlement temporary file; manual review required");
        J current=nullptr;
        if(std::filesystem::exists(path)){
            std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Settlement reread failed");
            current=J::parse(in);
        }
        if(current!=savedFile)throw std::runtime_error("Stale settlement snapshot; reload and review required");
        const J file{{"schema",1},{"mode","offline-prototype"},{"episodes",next},{"mail_sends",nextMail}};
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<file.dump(2);out.flush();
         if(!out)throw std::runtime_error("Settlement write failed");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Settlement commit failed; no request returned");
        rows=next;
        mailSends=nextMail;
        savedFile=file;
    }
    void commit(const J& next){commit(next,mailSends);}
    bool mailBatchPaid(const J& b)const{
        return b.at("unit")==0||(!b.at("episode").is_null()&&
            rows.at(b.at("episode").get<std::string>()).at("state")=="confirmed");
    }
    bool mailBatchOpen(const std::string& id,const J& b)const{
        if(b.contains("closed"))return !b.at("closed").get<bool>();
        // Backward-compatible: retained unresolved batches must not disappear
        // when a different batch becomes the account's most recent request.
        return !mailBatchPaid(b)||b.at("submission").is_null()||
            (mailSends.at("accounts").contains(b.at("account").get<std::string>())&&
             mailSends.at("accounts").at(b.at("account").get<std::string>())==id);
    }
public:
    struct MailSendPlan{std::string batch;unsigned units=0;uint64_t submission=0;};
    explicit DeferredCreditSettlement(std::filesystem::path file):path(std::move(file)){
        auto temp=path;temp+=L".tmp";
        if(std::filesystem::exists(temp))throw std::runtime_error("Interrupted settlement commit; manual review required");
        if(!std::filesystem::exists(path))return;
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Settlement read failed");
        const auto j=J::parse(in);
        if(j.at("schema")!=1||j.at("mode")!="offline-prototype"||!j.at("episodes").is_object())
            throw std::runtime_error("Unsupported settlement ledger");
        rows=j.at("episodes");
        if(j.contains("mail_sends")){
            mailSends=j.at("mail_sends");
            if(!mailSends.at("next").is_number_unsigned()||mailSends.at("next")==0||
               !mailSends.at("accounts").is_object()||!mailSends.at("batches").is_object())
                throw std::runtime_error("Invalid mail-send store");
            for(const auto& batch:mailSends.at("batches")){
                key(batch.at("account").get<std::string>());
                if(!batch.at("records").is_array()||batch.at("records").empty()||
                   !batch.at("unit").is_number_unsigned()||batch.at("unit").get<uint64_t>()>32767)
                    throw std::runtime_error("Invalid mail-send batch");
                if(batch.contains("closed")&&!batch.at("closed").is_boolean())
                    throw std::runtime_error("Invalid mail-send lifecycle");
                if(!batch.at("episode").is_null()&&!rows.contains(batch.at("episode").get<std::string>()))
                    throw std::runtime_error("Missing mail-send receipt");
                if(!batch.at("submission").is_null()&&(!batch.at("submission").is_number_unsigned()||batch.at("submission")==0))
                    throw std::runtime_error("Invalid mail-send submission");
            }
            for(auto it=mailSends.at("accounts").begin();it!=mailSends.at("accounts").end();++it)
                if(!mailSends.at("batches").contains(it.value().get<std::string>())||
                   mailSends.at("batches").at(it.value().get<std::string>()).at("account")!=it.key())
                    throw std::runtime_error("Invalid active mail-send account");
        }
        savedFile=j;
        for(auto it=rows.begin();it!=rows.end();++it){
            key(it.key());const auto& r=it.value();key(r.at("owner").get<std::string>());
            const auto amount=r.at("amount");
            if(!amount.is_number_integer()||amount.get<int64_t>()<0||amount.get<int64_t>()>32767)
                throw std::runtime_error("Invalid settlement amount");
            cardJSON(card(r.at("initial_card")));
            const std::string state=r.at("state");
            if(r.contains("allow_exhausted_shortfall")&&!r.at("allow_exhausted_shortfall").is_boolean())
                throw std::runtime_error("Invalid exhausted-card policy");
            if(state!="pending"&&state!="issued"&&state!="confirmed"&&state!="exhausted"&&state!="uncertain"&&state!="blocked")
                throw std::runtime_error("Invalid settlement state");
            if(state=="blocked")key(r.at("reason").get<std::string>());
            if(state!="pending"&&state!="blocked")key(r.at("connection").get<std::string>());
            if(state=="issued"){
                // Persisted issued intent could have reached ROM. Never replay it.
                it.value()["state"]="uncertain";it.value()["reason"]="process-restarted-after-issue";
            }
            if(state=="exhausted"){
                media_card::ServiceDebitExchange proof;proof.arm(amount.get<uint32_t>(),1,true);
                proof.request(card(r.at("initial_card")),0);
                const auto reply=r.at("reply").get<std::vector<uint8_t>>();proof.receive(reply,0);
                if(!r.value("allow_exhausted_shortfall",false)||!proof.exhaustedShortfall()||
                   r.at("consumed")!=*proof.result||r.at("updated_card")!=cardJSON(*proof.updatedCard))
                    throw std::runtime_error("Exhausted settlement lacks native reply proof");
            }
        }
    }
    J snapshot()const{std::lock_guard lock(mutex);return rows;}
    bool mailSendAlreadyPaid(const std::string& account,const J& records)const{
        // Read-only preflight exception: an exact confirmed retry needs no new
        // credit, including paid-before-journal recovery with a zero card.
        key(account);std::lock_guard lock(mutex);
        if(records.empty())return false;
        for(auto it=mailSends.at("batches").begin();it!=mailSends.at("batches").end();++it)
            if(it.value().at("account")==account&&mailBatchOpen(it.key(),it.value())&&
               it.value().at("records")==records&&mailBatchPaid(it.value()))return true;
        return false;
    }
    // The wire has no unique sent-letter ID. Keep every open outbox multiset
    // through retries/restarts, not only the account's latest batch. Empty
    // outboxes close paid/accepted batches. Only wholly unbound reservations
    // may grow; overlap with bound/paid batches remains ambiguous.
    MailSendPlan planMailSend(const std::string& account,const J& records,unsigned unit){
        key(account);if(!records.is_array()||unit>32767)throw std::invalid_argument("Invalid mail-send plan");
        std::lock_guard lock(mutex);auto next=mailSends;
        if(records.empty()){
            bool changed=false;
            for(auto it=next["batches"].begin();it!=next["batches"].end();++it)
                if(it.value().at("account")==account&&mailBatchOpen(it.key(),it.value())&&
                   mailBatchPaid(it.value())&&!it.value().at("submission").is_null()){
                    it.value()["closed"]=true;changed=true;
                    if(next["accounts"].contains(account)&&next["accounts"].at(account)==it.key())next["accounts"].erase(account);
                }
            if(changed)commit(rows,next);
            return {};
        }
        std::vector<std::string> superseded;
        for(auto it=next["batches"].begin();it!=next["batches"].end();++it){
            const auto id=it.key();const auto& b=it.value();
            if(b.at("account")!=account||!mailBatchOpen(id,b))continue;
            if(b.at("records")==records){
                if(!b.at("episode").is_null()&&!mailBatchPaid(b))
                    throw std::runtime_error("Unresolved prior mail debit; no retry or new consumption");
                const auto cost=uint64_t(records.size())*b.at("unit").get<unsigned>();
                if(cost>32767)throw std::runtime_error("Mail-send amount exceeds envelope");
                return {id,mailBatchPaid(b)?0u:unsigned(cost),b.at("submission").is_null()?0:b.at("submission").get<uint64_t>()};
            }
            // A mere unissued outbox reservation is not an uncertain debit.
            // Actual issued/uncertain consumption on this account still needs
            // review before any new wire debit (also enforced by issue()).
            if(!b.at("episode").is_null()&&!mailBatchPaid(b)&&
               rows.at(b.at("episode").get<std::string>()).at("state")!="pending")
                throw std::runtime_error("Previous outbox is unresolved");
            bool overlap=false;
            for(const auto& item:records)for(const auto& prior:b.at("records"))if(item==prior)overlap=true;
            if(overlap){
                // Ordinary queue growth: old+new letters, before ANY debit was
                // bound. Supersede the reservation, retain its audit record,
                // and charge the full current multiset exactly once.
                auto remaining=records;
                bool includesPrior=true;
                for(const auto& prior:b.at("records")){
                    auto found=std::find(remaining.begin(),remaining.end(),prior);
                    if(found==remaining.end()){includesPrior=false;break;}
                    remaining.erase(found);
                }
                if(b.at("episode").is_null()&&b.at("submission").is_null()&&b.at("unit")==unit&&includesPrior)
                    superseded.push_back(id);
                else throw std::runtime_error("Ambiguous overlapping outbox; no repeated charge");
            }
        }
        if(records.empty())return {};
        const uint64_t cost=uint64_t(records.size())*unit;
        if(cost>32767)throw std::runtime_error("Mail-send amount exceeds envelope");
        const auto sequence=next.at("next").get<uint64_t>();if(sequence==UINT64_MAX)throw std::runtime_error("Mail-send sequence exhausted");
        const auto id=newMailBatchKey(sequence);next["next"]=sequence+1;
        for(const auto& prior:superseded){next["batches"][prior]["closed"]=true;next["batches"][prior]["superseded_by"]=id;}
        next["accounts"][account]=id;next["batches"][id]={{"account",account},{"records",records},{"unit",unit},{"episode",nullptr},{"submission",nullptr},{"closed",false}};
        commit(rows,next);return {id,unsigned(cost),0};
    }
    void bindMailSend(const std::string& batch,const std::string& episode){
        key(episode);std::lock_guard lock(mutex);auto next=mailSends;auto& b=next["batches"].at(batch);
        if(!rows.contains(episode)||rows.at(episode).at("state")!="pending"||!b.at("episode").is_null())
            throw std::runtime_error("Mail-send receipt already bound or unavailable");
        b["episode"]=episode;commit(rows,next);
    }
    void acceptedMailSend(const std::string& batch,uint64_t submission){
        if(!submission)throw std::invalid_argument("Invalid mail submission");
        std::lock_guard lock(mutex);auto next=mailSends;auto& b=next["batches"].at(batch);
        if(b.at("unit")!=0&&(b.at("episode").is_null()||rows.at(b.at("episode").get<std::string>()).at("state")!="confirmed"))
            throw std::runtime_error("Mail acceptance needs a confirmed send debit");
        if(!b.at("submission").is_null()){if(b.at("submission")!=submission)throw std::runtime_error("Mail submission conflict");return;}
        b["submission"]=submission;commit(rows,next);
    }
    bool queue(const std::string& episode,const std::string& owner,uint32_t amount,const media_card::CardReport& initial,
               bool allowExhaustedShortfall=false){
        key(episode);key(owner);if(amount>32767)throw std::invalid_argument("Settlement envelope0..32767");
        const auto initialJSON=cardJSON(initial);std::lock_guard lock(mutex);
        if(rows.contains(episode)){
            const auto& r=rows.at(episode);
            if(r.at("owner")!=owner||r.at("amount")!=amount||r.at("initial_card")!=initialJSON||
               r.value("allow_exhausted_shortfall",false)!=allowExhaustedShortfall){
                if(r.at("state")=="pending"){
                    auto next=rows;next[episode]["state"]="blocked";
                    next[episode]["reason"]="conflicting-episode-context";
                    next[episode]["conflicting_candidate"]={{"owner",owner},{"amount",amount},{"initial_card",initialJSON}};
                    commit(next);
                }
                throw std::runtime_error("Conflicting episode; no replacement or charge");
            }
            return false;
        }
        auto next=rows;next[episode]={{"owner",owner},{"amount",amount},{"initial_card",initialJSON},{"state","pending"}};
        if(allowExhaustedShortfall)next[episode]["allow_exhausted_shortfall"]=true;
        commit(next);return true;
    }
    std::vector<uint8_t> issue(const std::string& episode,const std::string& connection,const media_card::CardReport& current){
        key(connection);const auto currentJSON=cardJSON(current);std::lock_guard lock(mutex);
        const auto& r=rows.at(episode);
        if(r.at("state")!="pending")throw std::runtime_error("Issued/confirmed/uncertain settlement cannot be resent");
        for(auto it=rows.begin();it!=rows.end();++it)if(it.key()!=episode&&
            (it.value().at("state")=="issued"||it.value().at("state")=="uncertain"||it.value().at("state")=="blocked")){
            const auto prior=card(it.value().at("initial_card"));
            bool sameCard=true;for(unsigned i=0;i<8;++i)if((*prior.raw)[i]!=(*current.raw)[i])sameCard=false;
            if(it.value().at("owner")==r.at("owner")||sameCard)
                throw std::runtime_error("Owner/card has unresolved prior consumption");
        }
        // Strict report equality is a local prototype safety gate, not historical policy.
        if(currentJSON!=r.at("initial_card")||
           (current.value<r.at("amount").get<int32_t>()&&!r.value("allow_exhausted_shortfall",false)))
            throw std::runtime_error("Card changed or insufficient finite balance");
        media_card::ServiceDebitExchange exchange;exchange.arm(r.at("amount").get<uint32_t>(),1,true);
        auto wire=exchange.request(current,0);
        auto next=rows;next[episode]["state"]="issued";next[episode]["connection"]=connection;
        commit(next); // Durable intent precedes returning any sendable bytes.
        return wire;
    }
    bool acknowledge(const std::string& episode,const std::string& connection,std::span<const uint8_t> reply){
        std::lock_guard lock(mutex);const auto& r=rows.at(episode);
        if(r.at("state")!="issued"||r.at("connection")!=connection)
            throw std::runtime_error("Reply not bound to an issued connection");
        media_card::ServiceDebitExchange exchange;exchange.arm(r.at("amount").get<uint32_t>(),1,true);
        exchange.request(card(r.at("initial_card")),0);exchange.receive(reply,0);
        const bool verified=exchange.continuationProblem()==nullptr;
        const bool exhausted=r.value("allow_exhausted_shortfall",false)&&exchange.exhaustedShortfall();
        auto next=rows;next[episode]["reply"]=std::vector<uint8_t>(reply.begin(),reply.end());
        next[episode]["state"]=verified?"confirmed":exhausted?"exhausted":"uncertain";
        if(verified||exhausted){
            next[episode]["updated_card"]=cardJSON(*exchange.updatedCard);
            next[episode]["consumed"]=*exchange.result;
        }
        else next[episode]["reason"]="reply-does-not-prove-requested-consumption";
        // Exhaustion is a known partial debit, NOT proof that the full request
        // was paid. The receipt is terminal/non-replayable; callers still deny
        // ordinary service and retain requested vs actual units separately.
        commit(next);return verified;
    }
    void interrupt(const std::string& episode){
        std::lock_guard lock(mutex);if(rows.at(episode).at("state")!="issued")return;
        auto next=rows;next[episode]["state"]="uncertain";next[episode]["reason"]="connection-interrupted";commit(next);
    }
};
}
