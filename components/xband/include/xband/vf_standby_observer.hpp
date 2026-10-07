#pragma once
#include <cstdint>
#include <limits>
namespace xband {
// Interpretation only. Caller obtains snapshots with read-only guest RAM peeks.
// Never converts guest clock units to host time or edits guest state.
// Shared XOS image 49ec092d... is identical in the eight local SegaNet titles.
// The historical VF name is kept below for source compatibility only.
class XOSStandbyObserver {
public:
    enum class Notice { none, ready, cancel };
    struct Sample {
        uint32_t clock,start,duration;
        bool active,monitor,serviceCarrier,peerStarting,romVerified;
    };
private:
    uint64_t generation_=0,ticket_=0;
    uint64_t consumedGeneration_=0,consumedTicket_=0;
    bool seenActive_=false,ready_=false,cancel_=false;
public:
    void reset(){*this=XOSStandbyObserver{};}
    // A successful modem handoff consumes this wait registration. Its ROM/RAM
    // may be replaced by the game; later peer closure must not resume monitoring
    // that old wait, even if control repeats the paired ticket until cleanup.
    void peerConnected(){
        if(ticket_){consumedGeneration_=generation_;consumedTicket_=ticket_;}
        ticket_=0;seenActive_=ready_=cancel_=false;
    }
    void arm(uint64_t generation,uint64_t ticket,bool localCaller=false){
        // A caller reply has no receiver wait duration. Do not interpret its
        // transient active flag / zero duration as a cancelled receiver ticket.
        if(localCaller)ticket=0;
        if(ticket&&generation==consumedGeneration_&&ticket==consumedTicket_)ticket=0;
        if(generation_==generation&&ticket_==ticket)return;
        generation_=generation;ticket_=ticket;seenActive_=ready_=cancel_=false;
    }
    uint64_t ticket()const{return ticket_;}
    uint64_t generation()const{return generation_;}
    Notice observe(const Sample& s){
        if(!ticket_||cancel_||s.peerStarting)return Notice::none;
        if(!s.romVerified){cancel_=true;return Notice::cancel;}
        seenActive_|=s.active;
        if(!seenActive_)return Notice::none; // Service reply may not have arrived yet.
        // Reject overflow instead of inventing wrap semantics for the original
        // unsigned deadline comparison. Original zero value is NOT indefinite.
        if(!s.duration||s.start>std::numeric_limits<uint32_t>::max()-s.duration||
           s.clock>=s.start+s.duration||!s.active||(ready_&&!s.monitor)){
            cancel_=true;return Notice::cancel;
        }
        if(!ready_&&s.monitor&&!s.serviceCarrier){ready_=true;return Notice::ready;}
        return Notice::none;
    }
};
using VFStandbyObserver = XOSStandbyObserver;
}
