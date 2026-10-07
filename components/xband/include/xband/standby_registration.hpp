#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <limits>

namespace xband {
// Two local endpoints, not authentication or a public subscriber directory.
// Time is supplied explicitly by the caller in ONE monotonic clock domain.
// No conversion between guest ticks and wall time is assumed here.
class StandbyRegistration {
public:
    enum class State { waiting, ready, paired };
    enum class Result { receiver, defer, caller };
    struct Entry {
        std::string phone;
        uint32_t game;
        uint64_t ticket,deadline;
        State state=State::waiting;
    };
    struct Decision { Result result; uint64_t ticket; unsigned caller=2,callee=2; };
private:
    std::array<std::optional<Entry>,2> entries_{};
    uint64_t next_=0,clock_=0;
    static void sideCheck(unsigned s){if(s>1)throw std::invalid_argument("Invalid standby endpoint");}
    void advance(uint64_t now){
        if(now<clock_)throw std::invalid_argument("Standby clock moved backwards");
        clock_=now;
        for(auto& e:entries_)if(e&&e->state!=State::paired&&now>=e->deadline)e.reset();
    }
    static std::string number(const std::string& input){
        std::string out;
        for(auto c:input){if(c>='0'&&c<='9')out+=c;else if(c!='-')throw std::invalid_argument("Invalid standby phone");}
        if(out.empty())throw std::invalid_argument("Empty standby phone");
        return out;
    }
public:
    const std::optional<Entry>& entry(unsigned s)const{sideCheck(s);return entries_[s];}
    void expire(uint64_t now){advance(now);}
    Decision request(unsigned side,const std::string& phone,uint32_t game,uint64_t now,uint64_t lifetime,bool matchAllowed=true){
        sideCheck(side);const auto normalized=number(phone);
        if(!lifetime||lifetime>std::numeric_limits<uint64_t>::max()-now)
            throw std::invalid_argument("Invalid standby lifetime");
        advance(now);auto& own=entries_[side];auto& peer=entries_[1-side];
        if(peer&&peer->phone==normalized)throw std::invalid_argument("Duplicate standby phone");
        if((own&&own->state==State::paired)||(peer&&peer->state==State::paired))
            throw std::logic_error("Paired registrations need call lifecycle completion");
        if(own&&(own->phone!=normalized||own->game!=game))
            throw std::logic_error("Cancel old registration before changing identity/game");
        if(!own){
            if(next_==std::numeric_limits<uint64_t>::max())throw std::overflow_error("Standby ticket exhausted");
            own=Entry{normalized,game,++next_,now+lifetime};
        } // A repeated service poll must NOT extend the original deadline.
        // A caller may retain a named wait while a non-target endpoint is
        // present. Allocate its ticket, but do not pair/defer against that peer.
        if(matchAllowed&&peer&&peer->game==game){
            if(peer->state!=State::ready)return {Result::defer,own->ticket};
            own->state=peer->state=State::paired;
            return {Result::caller,own->ticket,side,1-side};
        }
        return {Result::receiver,own->ticket};
    }
    // Invoke only after an independently observed service-close/readiness event.
    // A response being queued is not proof the guest entered standby.
    bool ready(unsigned side,uint64_t ticket,uint64_t now){
        sideCheck(side);advance(now);auto& e=entries_[side];
        if(!e||e->ticket!=ticket||e->state==State::paired)return false;
        e->state=State::ready;return true;
    }
    bool cancel(unsigned side,uint64_t ticket,uint64_t now){
        sideCheck(side);advance(now);auto& e=entries_[side];
        if(!e||e->ticket!=ticket)return false;
        if(e->state==State::paired)throw std::logic_error("Cancel cannot terminate an established call");
        e.reset();return true;
    }
    bool finish(unsigned side,uint64_t ticket,uint64_t now){
        sideCheck(side);advance(now);const auto& e=entries_[side];
        if(!e||e->ticket!=ticket||e->state!=State::paired)return false;
        entries_={};return true;
    }
};
}
