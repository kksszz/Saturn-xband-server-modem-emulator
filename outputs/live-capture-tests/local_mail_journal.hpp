#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "diagnostic_outgoing_request.hpp"
#include "diagnostic_mail_forward.hpp"
#include "diagnostic_mail_date.hpp"
#include "diagnostic_mail_region.hpp"
#include "native_mailbox.hpp"
#include "server_broadcast_mail.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <set>
namespace diagnostic {
// Persistent LOCAL observations, not authenticated accounts, custody receipts,
// or delivery. One immutable file per complete submitted request. Repeated
// equal requests remain separate observations: the wire has no unique mail ID.
// No configured total-mail limit; disk/OS failures refuse a new commit.
class LocalMailJournal {
    using J=nlohmann::json;
    std::filesystem::path root;
    uint64_t next=1;
    size_t mailCount=0;
    uint64_t nextOffer=1;
    uint64_t nextBroadcast=1;
    std::vector<J> broadcasts;
    std::map<std::string,std::string> offerStates;
    mutable std::recursive_mutex mutex;
    using Key=MailAccountKey;
    std::map<Key,std::vector<uint8_t>> names;
    std::map<Key,Key> phoneAccounts;
    // Current registered terminal for each of the two local modem endpoints.
    // Keep historical names for audit/source metadata, not as live recipients.
    std::map<unsigned,std::string> currentTerminals;
    struct MailboxReport { size_t reported=0,prepared=0,remaining=0; };
    mutable std::map<Key,MailboxReport> mailboxReports;
    static J read(const std::filesystem::path& path){
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Mail journal read failed");
        J j;in>>j;
        if(j.at("version")!=1||!j.at("mails").is_array())throw std::runtime_error("Invalid mail journal version/records");
        const auto raw=j.at("request").get<LocalTCPProbe::Bytes>();
        if(j.at("side").get<unsigned>()>1||j.at("profile").get<unsigned>()>3||j.at("delivery_confirmed")!=false)
            throw std::runtime_error("Invalid journal endpoint/profile/state");
        (void)currentName(raw);
        const auto code=LocalTCPProbe::serviceRequestCode(raw);
        if(code!=2&&code!=3&&code!=4)throw std::runtime_error("Invalid journal request");
        const auto records=decodeObservedOutgoingRequest(raw,false,true);
        if(j.at("phone")!=xband::registrationPhone(raw)||j.at("profile")!=raw[xband::registrationOffset(raw,43)]||records.size()!=j.at("mails").size())
            throw std::runtime_error("Journal metadata mismatch");
        (void)mailAccountKey(raw);
        for(size_t i=0;i<records.size();++i){
            if(j.at("mails")[i].at("player")!=records[i].prefix[0]||j.at("mails")[i].at("fields")!=J(records[i].fields)||j.at("mails")[i].at("state")!="pending_unverified")
                throw std::runtime_error("Journal record mismatch");
            const auto& row=j.at("mails")[i];
            if(row.contains("source_name")){
                const auto name=row.at("source_name").get<std::vector<uint8_t>>();
                if(name.empty()||name.size()>32||std::find(name.begin(),name.end(),0)!=name.end())throw std::runtime_error("Invalid frozen mail sender");
            }
            if(row.contains("target_account")){
                const auto key=row.at("target_account").get<Key>();
                if(key.first.empty()||key.second>3)throw std::runtime_error("Invalid frozen mail recipient");
            }
        }
        return j;
    }
    static std::vector<uint8_t> currentName(const LocalTCPProbe::Bytes& raw){
        const auto start=xband::registrationOffset(raw,81);const auto limit=std::min(raw.size(),start+16);
        const auto end=std::find(raw.begin()+start,raw.begin()+limit,0);
        if(end==raw.begin()+limit||end==raw.begin()+start)throw std::runtime_error("Invalid journal current name");
        return {raw.begin()+start,end};
    }
    void learn(const J& j){
        const auto raw=j.at("request").get<LocalTCPProbe::Bytes>();const auto name=currentName(raw);
        const auto key=mailAccountKey(raw);
        names[key]=name; // Same terminal/slot may rename; other slots are untouched.
        phoneAccounts[{xband::phoneDigits(j.at("phone").get<std::string>()),key.second}]=key;
        currentTerminals[j.at("side").get<unsigned>()]=key.first;
    }
    std::vector<std::filesystem::path> files()const{
        std::vector<std::filesystem::path> list;
        for(const auto& e:std::filesystem::directory_iterator(root))if(e.path().extension()==L".json")list.push_back(e.path());
        std::sort(list.begin(),list.end());return list;
    }
    static std::string keyFor(const J& j,size_t index){return std::to_string(j.at("id").get<uint64_t>())+":"+std::to_string(index);}
    static std::string text(const std::vector<uint8_t>& bytes){
        const auto end=std::find(bytes.begin(),bytes.end(),0);const auto size=int(end-bytes.begin());if(!size)return {};
        UINT code=51932;int n=MultiByteToWideChar(code,0,reinterpret_cast<const char*>(bytes.data()),size,nullptr,0);
        if(!n){code=20932;n=MultiByteToWideChar(code,0,reinterpret_cast<const char*>(bytes.data()),size,nullptr,0);}
        if(!n)return "[EUC-JP conversion unavailable; see raw fields]";
        std::wstring wide(n,L'\0');MultiByteToWideChar(code,0,reinterpret_cast<const char*>(bytes.data()),size,wide.data(),n);
        const auto length=WideCharToMultiByte(CP_UTF8,0,wide.data(),n,nullptr,0,nullptr,nullptr);std::string utf8(length,'\0');
        WideCharToMultiByte(CP_UTF8,0,wide.data(),n,utf8.data(),length,nullptr,nullptr);return utf8;
    }
    static void writeNew(const std::filesystem::path& target,const J& j){
        const auto bytes=j.dump();auto pending=target;pending+=L".pending";
        const HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Mail state pending creation failed");
        DWORD written=0;const bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
        CloseHandle(file);if(!ok||!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Mail state commit failed; pending preserved");
    }
    void setStates(const std::vector<std::string>& keys,const std::string& state){
        if(keys.empty())return;
        if(nextOffer==UINT64_MAX)throw std::runtime_error("Mail offer sequence exhausted");
        std::ostringstream filename;filename<<std::setw(20)<<std::setfill('0')<<nextOffer<<".offers";
        writeNew(root/filename.str(),J{{"version",1},{"id",nextOffer},{"state",state},{"keys",keys},{"confirmed",false}});
        ++nextOffer;for(const auto& key:keys)offerStates[key]=state;
    }
    std::optional<Key> target(std::span<const uint8_t> name)const{
        std::optional<Key> result;
        for(const auto& [key,value]:names){
            const bool current=std::any_of(currentTerminals.begin(),currentTerminals.end(),
                [&](const auto& endpoint){return endpoint.second==key.first;});
            if(current&&std::equal(value.begin(),value.end(),name.begin(),name.end())){
                if(result)return {};result=key; // Genuine current duplicates remain ambiguous.
            }
        }
        return result;
    }
public:
    explicit LocalMailJournal(std::filesystem::path path):root(std::move(path)){
        if(root.empty()||!root.is_absolute())throw std::runtime_error("Mail journal needs absolute directory");
        std::filesystem::create_directories(root);
        for(const auto& e:std::filesystem::directory_iterator(root))if(e.path().extension()==L".pending")
            throw std::runtime_error("Mail pending commit preserved; recover before startup");
        for(const auto& file:files()){
            const auto j=read(file);const auto id=j.at("id").get<uint64_t>();
            if(!id||id==UINT64_MAX)throw std::runtime_error("Invalid mail journal ID");
            std::ostringstream expected;expected<<std::setw(20)<<std::setfill('0')<<id<<".json";
            if(file.filename()!=expected.str())throw std::runtime_error("Journal ID/file mismatch");
            next=std::max(next,id+1);learn(j);mailCount+=j.at("mails").size();
        }
        std::vector<std::filesystem::path> offers;
        for(const auto& file:std::filesystem::directory_iterator(root))if(file.path().extension()==L".offers")offers.push_back(file.path());
        std::sort(offers.begin(),offers.end());
        for(const auto& path:offers){std::ifstream in(path);J j;in>>j;const auto id=j.at("id").get<uint64_t>();const auto state=j.at("state").get<std::string>();
            std::ostringstream expected;expected<<std::setw(20)<<std::setfill('0')<<id<<".offers";
            if(j.at("version")!=1||!id||id==UINT64_MAX||path.filename()!=expected.str()||j.at("confirmed")!=false||state!="prepared_unconfirmed")throw std::runtime_error("Invalid persisted mail offer state");
            for(const auto& key:j.at("keys"))offerStates[key.get<std::string>()]=state;
            nextOffer=std::max(nextOffer,id+1);
        }
        std::vector<std::filesystem::path> publications;
        for(const auto& e:std::filesystem::directory_iterator(root))if(e.path().extension()==L".broadcast")publications.push_back(e.path());
        std::sort(publications.begin(),publications.end());
        for(const auto& path:publications){std::ifstream in(path);J j;in>>j;
            const auto id=j.at("id").get<uint64_t>();std::ostringstream expected;expected<<std::setw(20)<<std::setfill('0')<<id<<".broadcast";
            if(j.at("version")!=1||!id||id==UINT64_MAX||path.filename()!=expected.str()||j.at("icon")!=201)
                throw std::runtime_error("Invalid broadcast publication");
            const auto phone=j.at("target_phone").get<std::string>();const auto slot=j.at("target_profile").get<int>();
            if(slot < -1||slot>3||(!phone.empty()&&xband::phoneDigits(phone)!=phone)||(phone.empty()&&slot!=-1))throw std::runtime_error("Invalid broadcast recipient");
            (void)broadcastRecord(j.at("subject_euc").get<RecordBytes>(),j.at("body_euc").get<RecordBytes>(),j.at("date").get<uint32_t>(),201);
            broadcasts.push_back(j);nextBroadcast=std::max(nextBroadcast,id+1);
        }
    }
    // Empty phone means all accounts, including accounts first seen later.
    // A terminal-wide publication is still offered independently to each slot.
    uint64_t publishBroadcast(const std::wstring& subject,const std::wstring& body,std::string phone="",int profile=-1){
        const auto title=broadcastEUC(subject,32),message=broadcastEUC(body,broadcastBodyMaxBytes);
        if(std::find(title.begin(),title.end(),10)!=title.end())throw std::invalid_argument("Subject must be one line");
        if(profile < -1||profile>3||(phone.empty()&&profile!=-1))throw std::invalid_argument("Invalid broadcast recipient");
        if(!phone.empty()){phone=xband::phoneDigits(phone);if(phone.empty()||phone.size()>20)throw std::invalid_argument("Invalid recipient phone");}
        std::lock_guard lock(mutex);if(nextBroadcast==UINT64_MAX)throw std::runtime_error("Broadcast ID exhausted");
        const auto now=std::chrono::system_clock::now();
        J j{{"version",1},{"id",nextBroadcast},{"target_phone",phone},{"target_profile",profile},{"icon",201},
            {"subject_euc",title},{"body_euc",message},{"date",mailAcceptanceDate(now)},
            {"accepted_unix_ms",std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()}};
        std::ostringstream name;name<<std::setw(20)<<std::setfill('0')<<nextBroadcast<<".broadcast";
        writeNew(root/name.str(),j);broadcasts.push_back(j);return nextBroadcast++;
    }
    J broadcastHistory()const{
        std::lock_guard lock(mutex);J rows=J::array();
        for(auto it=broadcasts.rbegin();it!=broadcasts.rend();++it){auto row=*it;
            row["subject"]=text(row.at("subject_euc").get<RecordBytes>());row["body"]=text(row.at("body_euc").get<RecordBytes>());
            const auto prefix="broadcast:"+std::to_string(row.at("id").get<uint64_t>())+":";
            size_t offered=0;for(const auto& [key,state]:offerStates)if(key.starts_with(prefix))++offered;
            row["prepared_accounts"]=offered;rows.push_back(std::move(row));}return rows;
    }
    uint64_t append(const LocalTCPProbe::Bytes& raw,unsigned side,const std::string& sendBatch={}){
        std::lock_guard lock(mutex);
        const auto code=LocalTCPProbe::serviceRequestCode(raw);
        if(side>1||(code!=2&&code!=3&&code!=4))throw std::invalid_argument("Journal requires complete mail/match request");
        const auto records=decodeObservedOutgoingRequest(raw,false,true);
        if(!sendBatch.empty()){
            if(sendBatch.size()>256||records.empty())throw std::invalid_argument("Invalid paid mail batch");
            for(const auto& file:files()){
                const auto previous=read(file);
                if(previous.value("send_batch",std::string{})!=sendBatch)continue;
                const auto old=previous.at("request").get<LocalTCPProbe::Bytes>();
                if(mailAccountKey(old).first!=mailAccountKey(raw).first||outgoingMailBatch(old)!=outgoingMailBatch(raw))
                    throw std::runtime_error("Paid mail batch/journal conflict");
                return previous.at("id").get<uint64_t>(); // No duplicate delivery entry.
            }
        }
        const auto profile=raw[xband::registrationOffset(raw,43)];if(profile>3)throw std::invalid_argument("Invalid journal profile");
        (void)currentName(raw);if(next==UINT64_MAX)throw std::runtime_error("Journal ID exhausted");
        const auto current=mailAccountKey(raw);const auto name=currentName(raw);
        J mails=J::array();for(const auto& record:records){
            J row{{"player",record.prefix[0]},{"fields",record.fields},{"state","pending_unverified"}};
            // Freeze known metadata before rename. Do not assign the active
            // user's name to another user's shared-outbox submission.
            const Key sender{current.first,record.prefix[0]};
            if(sender==current)row["source_name"]=name;
            else if(const auto it=names.find(sender);it!=names.end())row["source_name"]=it->second;
            const auto& to=record.fields[0];
            if(to.size()>1&&to.back()==0){
                const auto resolved=target(std::span<const uint8_t>(to).first(to.size()-1));
                if(resolved)row["target_account"]=*resolved;
            }
            mails.push_back(std::move(row));
        }
        J j{{"version",1},{"id",next},{"side",side},{"phone",xband::registrationPhone(raw)},{"profile",profile},
            {"accepted_unix_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
            {"request",raw},{"mails",mails},{"delivery_confirmed",false}};
        if(!sendBatch.empty())j["send_batch"]=sendBatch;
        const auto bytes=j.dump();std::ostringstream filename;filename<<std::setw(20)<<std::setfill('0')<<next<<".json";
        const auto target=root/filename.str();auto pending=target;pending+=L".pending";
        const HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Journal pending creation failed");
        DWORD written=0;const bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
        CloseHandle(file);
        // Preserve pending after any failure; never acknowledge lost data.
        if(!ok||!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Journal commit failed; pending preserved");
        const auto id=next++;learn(j);mailCount+=records.size();return id;
    }
    std::optional<std::vector<uint8_t>> source(const std::string& phone,unsigned player)const{
        std::lock_guard lock(mutex);
        const auto alias=phoneAccounts.find({xband::phoneDigits(phone),player});
        if(alias==phoneAccounts.end())return {};
        const auto it=names.find(alias->second);return it==names.end()?std::nullopt:std::optional{it->second};
    }
    J summary()const{
        std::lock_guard lock(mutex);
        J reports=J::array();for(const auto& [key,value]:mailboxReports)reports.push_back({{"account",key.first},{"profile",key.second},
            {"capacity",nativeInboxCapacity},{"reported",value.reported},{"prepared",value.prepared},{"remaining",value.remaining}});
        return {{"enabled",true},{"storage","disk-journal"},{"submissions",next-1},{"mails",mailCount},{"broadcasts",broadcasts.size()},{"prepared_unconfirmed",offerStates.size()},
            {"delivery_confirmed",false},{"server_retry",false},{"source_slots",4},{"inbox_capacity",nativeInboxCapacity},
            {"mailboxes",reports},{"clear_policy","after whole-request commit"}};
    }
    struct Inbox { LocalTCPProbe::Bytes wire;std::vector<std::string> keys;size_t reported=0,remaining=0; };
    Inbox prepareInbox(const LocalTCPProbe::Bytes& request)const{
        std::lock_guard lock(mutex);Inbox result;
        const auto current=mailAccountKey(request);
        result.reported=reportedInboxCount(request);const auto free=availableInboxSlots(request);
        const auto known=names.find(current);
        if(known==names.end()||known->second!=currentName(request))return result;
        std::vector<IncomingRecord> incoming;
        const auto phone=xband::phoneDigits(xband::registrationPhone(request));
        for(const auto& j:broadcasts){
            if(!j.at("target_phone").get<std::string>().empty()&&j.at("target_phone")!=phone)continue;
            if(j.at("target_profile").get<int>()>=0&&j.at("target_profile")!=current.second)continue;
            const auto key="broadcast:"+std::to_string(j.at("id").get<uint64_t>())+":"+current.first+":"+std::to_string(current.second);
            if(offerStates.contains(key))continue;
            if(incoming.size()>=free){++result.remaining;continue;}
            incoming.push_back(broadcastRecord(j.at("subject_euc").get<RecordBytes>(),j.at("body_euc").get<RecordBytes>(),j.at("date").get<uint32_t>(),201));result.keys.push_back(key);
        }
        for(const auto& file:files()){const auto j=read(file);for(size_t index=0;index<j.at("mails").size();++index){
            const auto key=keyFor(j,index);
            const auto state=offerStates.find(key);if(state!=offerStates.end()&&state->second=="prepared_unconfirmed")continue;
            const auto& row=j.at("mails")[index];
            const auto sender=row.contains("source_name")?std::optional{row.at("source_name").get<std::vector<uint8_t>>()}:
                source(j.at("phone").get<std::string>(),row.at("player").get<unsigned>());
            if(!sender)continue;
            const auto fields=row.at("fields").get<std::array<std::vector<uint8_t>,3>>();const auto& to=fields[0];
            if(to.size()<2||to.back()!=0)continue;
            const auto recipient=row.contains("target_account")?std::optional{row.at("target_account").get<Key>()}:
                target(std::span<const uint8_t>(to).first(to.size()-1));
            if(recipient!=std::optional<Key>{current})continue;
            if(incoming.size()>=free){++result.remaining;continue;}
            xband::MailCapture capture{};capture.sourceCodename=*sender;capture.fields=fields;
            capture.sourcePhone=j.at("phone").get<std::string>();
            capture.serverAcceptedDate=mailAcceptanceDate(std::chrono::system_clock::time_point(std::chrono::milliseconds(j.at("accepted_unix_ms").get<int64_t>())));
            const auto address=activeRegions?mailSenderAddress(*activeRegions,capture.sourcePhone):std::vector<uint8_t>{};
            incoming.push_back(incomingFromCapture(capture,address));result.keys.push_back(key);
        }}
        mailboxReports[current]={result.reported,result.keys.size(),result.remaining};
        if(!incoming.empty()){result.wire=encodeIncomingRecords(incoming);result.wire.push_back(2);}return result;
    }
    void markPrepared(const std::vector<std::string>& keys){std::lock_guard lock(mutex);setStates(keys,"prepared_unconfirmed");}
    J page(size_t pageIndex,size_t pageSize=20)const{
        std::lock_guard lock(mutex);
        if(!pageSize||pageSize>100)throw std::invalid_argument("Invalid history page size");
        J rows=J::array();size_t offset=0;const auto list=files();
        for(auto it=list.rbegin();it!=list.rend();++it){const auto j=read(*it);
            for(size_t i=0;i<j.at("mails").size();++i){
                if(offset++/pageSize!=pageIndex)continue;
                auto row=j.at("mails")[i];row["submission_id"]=j.at("id");row["record_index"]=i;row["phone"]=j.at("phone");row["accepted_unix_ms"]=j.at("accepted_unix_ms");
                const auto state=offerStates.find(keyFor(j,i));row["queue_state"]=state==offerStates.end()?"pending":state->second;
                const auto name=row.contains("source_name")?std::optional{row.at("source_name").get<std::vector<uint8_t>>()}:
                    source(j.at("phone").get<std::string>(),row.at("player").get<unsigned>());
                row["source_name"]=name?J(text(*name)):J(nullptr);
                const auto fields=row.at("fields").get<std::array<std::vector<uint8_t>,3>>();row["recipient"]=text(fields[0]);row["subject"]=text(fields[1]);
                const auto& to=fields[0];
                const auto recipient=row.contains("target_account")?std::optional{row.at("target_account").get<Key>()}:
                    (to.size()>1&&to.back()==0?target(std::span<const uint8_t>(to).first(to.size()-1)):std::nullopt);
                if(recipient){
                    const auto report=mailboxReports.find(*recipient);
                    if(report!=mailboxReports.end())row["recipient_mailbox"]={{"profile",recipient->second},{"capacity",nativeInboxCapacity},
                        {"reported",report->second.reported},{"prepared",report->second.prepared},{"remaining",report->second.remaining}};
                }
                std::ostringstream body;for(auto byte:fields[2])body<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte)<<' ';row["encoded_body_hex"]=body.str();
                row["delivery_confirmed"]=false;rows.push_back(std::move(row));
            }
        }
        return {{"page",pageIndex},{"page_size",pageSize},{"total_observations",offset},{"rows",rows},
            {"note","ROM resubmissions are separate submissions. After response preparation the server does not retry. Prepared is not a delivery/read receipt. Only the selected user's available inbox slots are offered; overflow remains on the server without a configured total limit."}};
    }
};
}
