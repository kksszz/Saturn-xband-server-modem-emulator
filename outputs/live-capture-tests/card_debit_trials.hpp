#pragma once
#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <cstdint>
#include <stdexcept>

namespace diagnostic {
// UI commands are consumed only by the service owner. Never retries after reset.
class CardDebitTrials {
public:
    enum class Kind { Arm, Continue };
    struct Command {Kind kind;uint64_t session;uint32_t amount=0;uint64_t timeoutMs=0;};
    struct Snapshot {
        uint64_t session=0;bool connected=false,pending=false;
        std::string phase="Disconnected",detail,last;
        uint32_t amount=0;std::optional<int64_t> result;std::optional<int32_t> reported,remaining;
        bool continuationVerified=false; // Published only by the service owner.
    };
private:
    mutable std::mutex mutex;
    std::array<Snapshot,2> rows;
    std::array<std::optional<Command>,2> commands;
    static void sideCheck(unsigned side){if(side>1)throw std::invalid_argument("Unknown trial endpoint");}
public:
    Snapshot snapshot(unsigned side)const{sideCheck(side);std::lock_guard lock(mutex);return rows[side];}
    void reserve(unsigned side,uint32_t amount,uint64_t timeoutMs){
        sideCheck(side);std::lock_guard lock(mutex);auto&r=rows[side];
        if(amount>32767||timeoutMs==0||commands[side]||(r.connected&&r.phase!="Idle"))
            throw std::runtime_error("Debit trial requires a fresh idle endpoint");
        // A reservation targets the next guest service call, not the idle
        // transport's current epoch. Dial initialization may reset that epoch.
        commands[side]=Command{Kind::Arm,0,amount,timeoutMs};
        r.pending=true;r.amount=amount;r.detail="Reserved once; not sent";
    }
    void continueTrial(unsigned side){
        sideCheck(side);std::lock_guard lock(mutex);auto&r=rows[side];
        if(commands[side]||!r.connected||r.phase!="Completed"||r.result!=int64_t(r.amount)||!r.continuationVerified)
            throw std::runtime_error("No matching debit and card balance verification");
        commands[side]=Command{Kind::Continue,r.session};r.pending=true;
    }
    uint64_t begin(unsigned side){
        sideCheck(side);std::lock_guard lock(mutex);auto&r=rows[side];
        ++r.session;r.connected=true;r.phase="Idle";r.result.reset();r.reported.reset();r.remaining.reset();r.detail.clear();
        r.continuationVerified=false;
        if(commands[side]&&commands[side]->session!=0){commands[side].reset();r.pending=false;r.detail="Stale command discarded";}
        return r.session;
    }
    std::optional<Command> take(unsigned side,uint64_t session,bool armReady=true){
        sideCheck(side);std::lock_guard lock(mutex);auto c=commands[side];
        if(!c)return {};
        if(c->kind==Kind::Arm&&!armReady)return {};
        if(c->kind==Kind::Arm&&c->session==0)c->session=session;
        commands[side].reset();rows[side].pending=false;
        if(c->session!=session){rows[side].detail="Stale command discarded";return {};}
        rows[side].detail.clear();
        return c;
    }
    void publish(unsigned side,uint64_t session,std::string phase,uint32_t amount,
                 std::optional<int64_t> result,std::optional<int32_t> reported,std::string detail={},std::optional<int32_t> remaining={},bool continuationVerified=false){
        sideCheck(side);std::lock_guard lock(mutex);auto&r=rows[side];
        if(r.session!=session||!r.connected)return;
        r.phase=std::move(phase);r.amount=commands[side]&&commands[side]->kind==Kind::Arm?commands[side]->amount:amount;r.result=result;
        r.continuationVerified=continuationVerified;
        if(reported)r.reported=reported;
        if(remaining)r.remaining=remaining;
        if(!detail.empty())r.detail=std::move(detail);
    }
    void finish(unsigned side,uint64_t session){
        sideCheck(side);std::lock_guard lock(mutex);auto&r=rows[side];
        if(r.session!=session||!r.connected)return;
        r.last="Session "+std::to_string(session)+": "+r.phase+
            (r.result?" result="+std::to_string(*r.result):" result not confirmed");
        r.connected=false;r.phase="Disconnected";
        if(commands[side]&&commands[side]->session==session){commands[side].reset();r.pending=false;}
    }
};
}
