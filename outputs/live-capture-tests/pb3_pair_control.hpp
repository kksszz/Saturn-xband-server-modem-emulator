#pragma once
#include <xband/service_endpoint.hpp>
#include <xband/async_modem_link.hpp>
#include <xband/local_match_roles.hpp>
#include <xband/standby_registration.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <iostream>
#include <algorithm>
#include <functional>
#include "pb3_replay_limit.hpp"
#include "activity_history.hpp"
// Migration-only control application carried over authenticated v2 data/advance.
// Separate endpoint/ports from guest PPP and game bytes; never injected into UART.
struct PB3TimeAdmissionDenied final:std::runtime_error {
    PB3TimeAdmissionDenied():std::runtime_error("Match request outside allowed JST window"){}
};
struct PB3PairControl {
    std::shared_ptr<xband::LocalMatchRoles> roles=std::make_shared<xband::LocalMatchRoles>();
    std::array<bool,2> joined{},finished{};
    std::array<bool,2> telephoneLine{true,true}; // Independent of modem power/card; volatile per server session.
    void setTelephoneLine(unsigned side,bool connected){
        if(side>1)throw std::invalid_argument("Invalid telephone line side");
        if(telephoneLine[side]==connected)return;
        telephoneLine[side]=connected;
        record(side,connected?"telephone_line_on":"telephone_line_off",
            connected?"電話線を接続。ゲーム側からの再接続が必要です。":"電話線を切断。モデムとカードは保持します。");
        if(connected)return;
        if(state==2&&creditCarrierEnded)creditCarrierEnded();
        if(state!=0||roles->caller<2){
            state=0;closing=true;roles->blocked=true;closedAck={};
        }else abortStandbyService(side);
        for(auto& q:bytes)q.clear();
    }
    std::array<bool,2> postMatchServed{}; // One observed post-call terminator per endpoint and generation.
    unsigned state=0; // 0 idle, 1 ringing, 2 connected
    uint64_t generation=1;
    bool failed=false;
    bool closing=false;
    // Opt-in control protocol only. Legacy clients keep the previous policy.
    bool standbyEnabled=false;
    std::array<bool,2> standbyCapable{};
    std::array<std::string,2> standbyProtocols{};
    xband::StandbyRegistration standby;
    std::array<std::string,2> standbyNames{}; // Local registration names, not authenticated users.
    std::array<std::string,2> standbyTargets{}; // Preserve a named wait; never match it to another user.
    std::function<bool(const std::string&)> matchAdmission; // Server wall-clock policy, independent of guest clocks.
    std::shared_ptr<diagnostic::ActivityHistory> activity;
    std::array<nlohmann::json,2> activityContext{nlohmann::json::object(),nlohmann::json::object()};
    std::function<void(uint64_t,const std::array<nlohmann::json,2>&)> creditCarrierStarted;
    std::function<void()> creditCarrierEnded;
    void record(unsigned side,const char* event,const std::string& detail={}){
        if(!activity)return;auto row=activityContext[side];row["side"]=side;row["event"]=event;
        row["generation"]=generation;row["detail"]=detail;
        // Freeze both participants at carrier establishment, before the next
        // service login can replace either endpoint's selected-user context.
        if(std::string(event)=="connected"){
            row["participants"]=nlohmann::json::array();
            for(unsigned i=0;i<2;++i){auto participant=activityContext[i];participant["side"]=i;row["participants"].push_back(std::move(participant));}
        }
        activity->append(std::move(row));
    }
    void abortStandbyService(unsigned side){
        if(side>1)throw std::runtime_error("Invalid standby abort side");
        const auto& e=standby.entry(side);if(!e)return;
        if(e->state==xband::StandbyRegistration::State::paired){
            state=0;closing=true;roles->blocked=true;closedAck={};
        }else{const auto ticket=e->ticket;standby.cancel(side,ticket,0);roles->withdraw(side);}
    }
    void enforceAdmission(){
        // A connected game may finish. Restrict new routes/carriers, not live
        // game bytes or result reporting, and never mix wall/guest clocks.
        if(!matchAdmission||state==2||closing)return;
        for(unsigned side=0;side<2;++side){
            const auto& e=standby.entry(side);
            const auto phone=e?e->phone:activityContext[side].value("phone",std::string{});
            if(phone.empty()||matchAdmission(phone))continue;
            if(state==1||roles->caller<2){
                state=0;closing=true;roles->blocked=true;closedAck={};
                record(side,"time_expired","利用時間外になったため、対戦開始前の接続世代を終了。接続済みの対戦は中断しません。");return;
            }
            if(e){const auto ticket=e->ticket;standby.cancel(side,ticket,0);roles->withdraw(side);
                record(side,"time_expired","利用時間外になったため、未成立の待機登録を終了。");}
        }
    }
    xband::StandbyRegistration::Decision registerStandby(unsigned side,const std::string& phone,uint32_t game,
                                                        const std::string& name={},const std::string& target={}){
        if(side>1)throw std::runtime_error("Invalid standby side");
        enforceAdmission();
        if(matchAdmission&&!matchAdmission(phone))throw PB3TimeAdmissionDenied{};
        // A new service login can race the old carrier-loss acknowledgements.
        // Wait for generation renewal rather than killing its transport.
        if(closing&&!failed&&telephoneLine[side]&&standbyEnabled&&standbyCapable[side])
            return {xband::StandbyRegistration::Result::defer,0};
        if(!telephoneLine[side]||!standbyEnabled||!standbyCapable[side]||failed||closing||state!=0||roles->caller<2)
            throw std::runtime_error("Standby control boundary not ready");
        // No target registration: complete service as a receiver so the
        // unmodified ROM owns the wait/expiry/notice62 lifecycle. Keep the
        // target constraint when the other endpoint subsequently logs in.
        if(!target.empty()){
            const auto& peer=standby.entry(1-side);
            // Legacy Matching_Specific resolves the challengee before it
            // checks game compatibility. A different local user is not the
            // requested opponent, even if that user has another game loaded.
            if(name==target||(peer&&standbyNames[1-side]==target&&peer->game!=game))
                throw std::runtime_error("Named rival is not the registered local opponent");
        }
        const auto& own=standby.entry(side);
        if(own&&(standbyNames[side]!=name||standbyTargets[side]!=target))
            throw std::runtime_error("Cancel old standby before changing name/target");
        const auto& peer=standby.entry(1-side);
        const bool matchAllowed=peer&&
            (target.empty()||standbyNames[1-side]==target)&&
            (standbyTargets[1-side].empty()||standbyTargets[1-side]==name);
        // Expiry is explicitly notified from EACH guest, never mix their clocks.
        // An endpoint disconnect removes its registration below.
        const auto priorTicket=standby.entry(side)?standby.entry(side)->ticket:0;
        const auto d=standby.request(side,phone,game,0,UINT64_MAX,matchAllowed);
        standbyNames[side]=name;
        standbyTargets[side]=target;
        roles->requested[side]=true;
        if(d.result==xband::StandbyRegistration::Result::caller){
            roles->caller=d.caller;roles->first=d.caller;
            record(side,"matched","Match route assigned; peer carrier is not established yet");
        }
        if(d.result==xband::StandbyRegistration::Result::receiver&&priorTicket!=d.ticket)
            record(side,"standby",target.empty()?std::string{}:"指名先を保持してゲームDISC(ROM)で待機。期限終了時の案内はゲームDISC(ROM)既存の通知0062。独自通知は送信しません。");
        return d;
    }
    std::array<bool,2> closedAck{};
    void renew(){
        if(generation==UINT64_MAX)throw std::runtime_error("Session generation exhausted");
        ++generation;state=0;closing=false;closedAck={};finished={};postMatchServed={};
        elapsed={};grantEnd={};sent={};received={};
        for(auto &q:bytes)q.clear();for(auto &q:recent)q.clear();
        asyncRelay=xband::AsyncModemRelay(generation);
        *roles=xband::LocalMatchRoles{};
        standby=xband::StandbyRegistration{};
        standbyNames={};
        standbyTargets={};
        std::cout<<"PAIR_SESSION_READY generation="<<generation<<'\n'<<std::flush;
    }
    bool asynchronous=false;
    xband::AsyncModemRelay asyncRelay{generation};
    const uint64_t stopAt=
#ifdef XBAND_FRONTEND_SERVER
        uint64_t{1}<<60;
#else
        pb3ReplayLimit();
#endif
    std::array<uint64_t,2> elapsed{},grantEnd{},sent{},received{};
    std::array<std::deque<uint8_t>,2> bytes;
    std::array<std::deque<uint8_t>,2> recent; // bounded, in-memory display only
    void observe(unsigned side,uint8_t b){recent[side].push_back(b);if(recent[side].size()>16)recent[side].pop_front();}
};
class PB3PairControlEndpoint final:public xband::ServiceEndpoint {
    std::shared_ptr<PB3PairControl> pair;
    unsigned side;
    bool used=false;
    std::string input;
    std::deque<uint8_t> output;
public:
    PB3PairControlEndpoint(std::shared_ptr<PB3PairControl> p,unsigned s):pair(std::move(p)),side(s){}
    bool transmit(uint8_t b,unsigned)override{if(input.size()>=4096)return false;input.push_back(char(b));return true;}
    void tick(unsigned)override{
        if(input.empty())return;
        if(!output.empty())throw std::runtime_error("pair control response not consumed");
        const auto request=nlohmann::json::parse(input);input.clear();
        auto op=request.at("op").get<std::string>();
        pair->enforceAdmission();
        // Keep transport alive long enough to report carrier loss to both games.
        // No data or new call is accepted after closure; a fresh session is required.
        if(pair->failed){if(!used)throw std::runtime_error("fresh session required");op="poll";}
        if(op!="join"&&op!="poll"&&request.contains("generation")){
            const auto token=request.at("generation").get<uint64_t>();
            if(token>pair->generation)throw std::runtime_error("Future session generation");
            if(token<pair->generation)op="poll"; // Delayed previous-match command: never mutate the new match.
        }
        if(pair->closing&&op!="closed_ack"&&used)op="poll";
        if((!pair->telephoneLine[0]||!pair->telephoneLine[1])&&
           (op=="dial"||op=="answer"||op=="exchange"||op=="step"))op="poll";
        if(!used){
            if(op!="join")throw std::runtime_error("pair requires join");
            const auto transport=request.value("transport",std::string("lockstep"));
            if(transport!="lockstep"&&transport!="async-v1")throw std::runtime_error("unknown pair transport");
            const bool asynchronous=transport=="async-v1";
            if(pair->joined[1-side]&&pair->asynchronous!=asynchronous)throw std::runtime_error("pair transport mismatch");
            pair->asynchronous=asynchronous;used=true;pair->joined[side]=true;
            pair->standbyProtocols[side]=request.value("standby_protocol",std::string{});
            pair->standbyCapable[side]=pair->standbyProtocols[side]=="xband-readonly-v1"||
                pair->standbyProtocols[side]=="vf-readonly-v1";
            pair->activityContext[side]=nlohmann::json::object();pair->record(side,"terminal_join");
        }
        std::vector<uint8_t> received;
        if(op=="standby_ready"||op=="standby_cancel"){
            if(!pair->standbyEnabled||!pair->standbyCapable[side]||!request.contains("generation")||
               request.at("generation")!=pair->generation)throw std::runtime_error("Unsupported standby notification");
            const auto ticket=request.at("ticket").get<uint64_t>();
            const auto& e=pair->standby.entry(side);
            const bool current=e&&e->ticket==ticket;
            if(op=="standby_ready"){
                const bool alreadyReady=current&&e->state==xband::StandbyRegistration::State::ready;
                if(pair->standby.ready(side,ticket,0)){
                    if(!alreadyReady)pair->record(side,"ready");
                    std::cout<<"STANDBY_READY_ACCEPTED side="<<side<<" ticket="<<ticket<<" generation="<<pair->generation<<'\n'<<std::flush;
                }
            }else if(current&&e->state==xband::StandbyRegistration::State::paired){
                // Timeout raced with peer role assignment: close the generation,
                // not just one registration or a caller already published.
                pair->state=0;pair->closing=true;pair->roles->blocked=true;pair->closedAck={};
                pair->record(side,"cancel","Wait ended after match assignment; closing generation");
            }else if(pair->standby.cancel(side,ticket,0)){
                pair->roles->withdraw(side);
                pair->record(side,"cancel","Guest reported timeout or cancellation");
                std::cout<<"STANDBY_CANCEL_ACCEPTED side="<<side<<" ticket="<<ticket<<" generation="<<pair->generation<<'\n'<<std::flush;
            }
        }else if(op=="closed_ack"){
            if(!pair->closing||request.at("generation")!=pair->generation)throw std::runtime_error("Unexpected close acknowledgement");
            pair->closedAck[side]=true;
            if(pair->closedAck[0]&&pair->closedAck[1])pair->renew();
        }else if(op=="exchange"){
            if(!pair->asynchronous||pair->state!=2||pair->finished[side])throw std::runtime_error("async exchange without carrier");
            const auto tx=request.at("bytes").get<std::vector<unsigned>>();
            std::vector<uint8_t> bytes;for(auto b:tx){if(b>255)throw std::runtime_error("invalid peer byte");bytes.push_back(uint8_t(b));}
            const auto elapsed=request.at("elapsed").get<uint64_t>();
            received=pair->asyncRelay.exchange(side,request.at("generation").get<uint64_t>(),request.at("sequence").get<uint64_t>(),elapsed,
                bytes,request.at("credit").get<size_t>());
            pair->elapsed[side]=elapsed;pair->sent[side]+=bytes.size();pair->received[side]+=received.size();
            for(auto b:bytes)pair->observe(side,b);
        }else if(op=="step"){
            if(pair->asynchronous)throw std::runtime_error("lockstep request in asynchronous session");
            if(pair->state!=2||pair->finished[side]||request.at("generation")!=pair->generation)
                throw std::runtime_error("step without current carrier");
            const auto elapsed=request.at("elapsed").get<uint64_t>();
            if(elapsed<pair->elapsed[side]||elapsed>pair->grantEnd[side]+64||
               elapsed>pair->elapsed[1-side]+4096+64)throw std::runtime_error("invalid guest progress");
            const auto tx=request.at("bytes").get<std::vector<unsigned>>();
            if(tx.size()>256||pair->bytes[1-side].size()+tx.size()>256)throw std::runtime_error("peer queue overflow");
            for(auto b:tx)if(b>255)throw std::runtime_error("invalid peer byte");
            pair->elapsed[side]=elapsed;
            for(auto b:tx)pair->bytes[1-side].push_back(static_cast<uint8_t>(b));
            pair->sent[side]+=tx.size();
            for(auto b:tx)pair->observe(side,static_cast<uint8_t>(b));
            auto &rx=pair->bytes[side];received.assign(rx.begin(),rx.end());rx.clear();
            pair->received[side]+=received.size();
            const auto limit=std::min(pair->stopAt,pair->elapsed[1-side]+4096);
            pair->grantEnd[side]=std::max(elapsed,std::min(elapsed+4096,limit));
        }else if(op=="dial"){
            if(side!=pair->roles->caller||pair->state!=0||!pair->joined[1-side]||request.at("number")!=(side?"3336666666":"3336666665"))
                throw std::runtime_error("invalid original peer dial");
            pair->state=1;
            pair->record(side,"dial");
            std::cout<<"PAIR_ORIGINAL_DIAL side="<<side<<" target="<<(side?"3336666666":"3336666665")<<" generation="<<pair->generation<<'\n'<<std::flush;
        }else if(op=="answer"){
            if(side!=pair->roles->callee()||pair->state!=1||request.at("generation")!=pair->generation)
                throw std::runtime_error("stale or unsolicited original answer");
            if(pair->creditCarrierStarted)pair->creditCarrierStarted(pair->generation,pair->activityContext);
            pair->state=2;
            pair->record(side,"connected","Peer carrier established; no gameplay completion or winner inference");
            std::cout<<"PAIR_ORIGINAL_ANSWER side="<<side<<" generation="<<pair->generation<<'\n'<<std::flush;
        }else if(op=="hangup"){
            if(request.at("generation")!=pair->generation)throw std::runtime_error("stale hangup");
            if(pair->creditCarrierEnded)pair->creditCarrierEnded();
            pair->state=0;pair->closing=true;pair->roles->blocked=true;pair->closedAck={};
            pair->record(side,"hangup","Guest modem hangup; not a game result verdict");
            std::cout<<"PAIR_HANGUP side="<<side<<" generation="<<pair->generation<<'\n'<<std::flush;
        }else if(op=="finish"){
            if(pair->state!=2||request.at("generation")!=pair->generation)throw std::runtime_error("finish without paired carrier");
            pair->finished[side]=true;
        }else if(op!="poll"&&op!="join")throw std::runtime_error("unknown pair control operation");
        auto responseValue=nlohmann::json{{"state",pair->state},{"generation",pair->generation},{"transport",pair->asynchronous?"async-v1":"lockstep"},
            {"closed",pair->failed||pair->closing},{"recoverable",!pair->failed},{"caller",pair->roles->caller},{"callee",pair->roles->callee()},{"joined",pair->joined},{"finished",pair->finished},{"grant_end",pair->grantEnd[side]},
            {"bytes",received},{"telephone_line",pair->telephoneLine}};
        if(pair->standbyEnabled&&pair->standbyCapable[side]){
            const auto& e=pair->standby.entry(side);
            responseValue["standby"]={{"protocol",pair->standbyProtocols[side]},{"ticket",e?e->ticket:0},
                {"state",e?unsigned(e->state):3},{"game",e?e->game:0}};
        }
        const auto response=responseValue.dump();
        for(unsigned char c:response)output.push_back(c);
    }
    bool peek(uint8_t &b)const override{if(output.empty())return false;b=output.front();return true;}
    void consume()override{if(output.empty())throw std::runtime_error("empty pair reply");output.pop_front();}
    size_t pending()const override{return output.size();}
    void reset()override{
        input.clear();output.clear();
        if(!used)return;
        if(pair->state==2&&pair->creditCarrierEnded)pair->creditCarrierEnded();
        pair->record(side,"terminal_leave",pair->state==2?"Transport left during peer connection":"Control transport disconnected");
        if(pair->standbyEnabled){
            const auto& e=pair->standby.entry(side);
            if(e&&e->state!=xband::StandbyRegistration::State::paired){
                const auto ticket=e->ticket;pair->standby.cancel(side,ticket,0);pair->roles->withdraw(side);
            }
        }
        // Mail/service calls do not require a game peer. Losing an idle modem
        // must not close the other modem's server session or poison a new join.
        const bool idle=pair->state==0&&!pair->closing&&pair->roles->caller==2&&
            (pair->standbyEnabled||(!pair->roles->requested[0]&&!pair->roles->requested[1]));
        pair->joined[side]=false;
        pair->standbyCapable[side]=false;
        pair->standbyProtocols[side].clear();
        if(!idle&&!(pair->finished[0]&&pair->finished[1])){
            // Transport loss ends only this carrier generation. The absent
            // endpoint cannot acknowledge it; a fresh join must remain possible.
            if(!pair->closing){pair->state=0;pair->closing=true;pair->roles->blocked=true;pair->closedAck={};}
            pair->closedAck[side]=true;
            if((pair->closedAck[0]||!pair->joined[0])&&(pair->closedAck[1]||!pair->joined[1]))pair->renew();
        }
        used=false;
    }
    ~PB3PairControlEndpoint(){reset();}
};
