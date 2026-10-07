#pragma once
#include <xband/at_command.hpp>
#include <xband/modem_escape.hpp>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xband {
// Observed AT/service session. No Ymir, sockets, PPP, game PC/RAM or card store.
// Hooks enqueue only; owner performs external work. Not a full Hayes model.
class ModemCommandSession {
public:
    enum class Dial {pending,busy,rejected};
    enum class State {command,dialing,data};
    struct Hooks {
        std::function<bool(std::string_view)> reply;
        std::function<bool(uint8_t)> payload;
        std::function<Dial(std::string_view)> dial;
        std::function<void()> hangup;
        std::function<void()> clearReceive;
        // Optional peer route; true means it owns the entire command.
        std::function<bool(const std::string&)> intercept;
        std::function<void(std::string_view)> observedCommand;
    };
    explicit ModemCommandSession(Hooks hooks):hooks_(std::move(hooks)){
        if(!hooks_.reply||!hooks_.payload||!hooks_.dial||!hooks_.hangup||!hooks_.clearReceive)
            throw std::invalid_argument("incomplete modem session hooks");
    }
    ModemCommandSession(const ModemCommandSession&)=delete;
    ModemCommandSession& operator=(const ModemCommandSession&)=delete;
    State state()const noexcept{return state_;}
    bool dataMode()const noexcept{return state_==State::data;}
    bool carrier()const noexcept{return carrier_;}
    uint64_t ticket()const noexcept{return ticket_;}
    std::string_view command()const noexcept{return line_;}
    const std::map<std::string,int>& profile()const noexcept{return profile_;}
    // Peer calls use the same command/data/escape state machine as service calls.
    void beginPeerDial(){
        if(state_!=State::command||carrier_)throw std::logic_error("Peer dial outside command mode");
        invalidate();state_=State::dialing;
    }
    void disconnected(unsigned frame){
        const bool notify=carrier_||state_==State::dialing;
        invalidate();state_=State::command;carrier_=false;line_.clear();escape_.reset(frame);
        hooks_.clearReceive();if(notify)emit("\r\nNO CARRIER\r\n");
    }
    // A delayed peer close belongs to the ticket that started that call, not
    // to a later service dial already issued by the guest.
    bool disconnectedIfCurrent(uint64_t ticket,unsigned frame){
        if(ticket!=ticket_)return false;
        disconnected(frame);return true;
    }
    // Owner matches the ticket captured when it queued the open request.
    bool connected(uint64_t ticket,unsigned frame){
        if(ticket!=ticket_||state_!=State::dialing)return false;
        carrier_=true;state_=State::data;escape_.reset(frame);emit("\r\nCONNECT 14400\r\n");return true;
    }
    void reset(unsigned frame){
        invalidate();hooks_.hangup();hooks_.clearReceive();
        state_=State::command;carrier_=false;line_.clear();profile_.clear();escape_.reset(frame);
    }
    void tick(unsigned frame){
        if(!dataMode())return;
        escape_.tick(frame);
        for(auto byte:escape_.out)if(!hooks_.payload(byte))throw std::runtime_error("modem payload queue full");
        escape_.out.clear();
        if(escape_.escaped){state_=State::command;line_.clear();emit("\r\nOK\r\n");}
    }
    void feed(uint8_t byte,unsigned frame){
        tick(frame);
        if(dataMode()){escape_.feed(byte,frame);tick(frame);return;}
        const auto event=appendATByte(line_,byte);
        if(event==ATLineEvent::overflow){emit("\r\nERROR\r\n");return;}
        if(event!=ATLineEvent::complete)return;
        const auto command=std::exchange(line_,{});
        if(hooks_.observedCommand)hooks_.observedCommand(command);
        if(state_==State::command&&!carrier_&&hooks_.intercept&&hooks_.intercept(command))return;
        const auto parsed=decodeObservedAT(command,true);
        switch(parsed.kind){
        case ATKind::attention:emit("\r\nOK\r\n");break;
        case ATKind::reset:reset(frame);emit("\r\nOK\r\n");break;
        case ATKind::hangup:{auto saved=profile_;reset(frame);profile_=std::move(saved);emit("\r\nOK\r\n");break;}
        case ATKind::profile:profile_=parsed.profile;emit("\r\nOK\r\n");break;
        case ATKind::dial:{
            if(state_!=State::command||carrier_){emit("\r\nERROR\r\n");break;}
            invalidate();const auto result=hooks_.dial(parsed.number);
            if(result==Dial::pending)state_=State::dialing;
            else emit(result==Dial::busy?"\r\nBUSY\r\n":"\r\nNO CARRIER\r\n");
            break;
        }
        default:emit("\r\nERROR\r\n");break;
        }
    }
private:
    void invalidate(){
        if(ticket_==std::numeric_limits<uint64_t>::max())throw std::overflow_error("modem session generation exhausted");
        ++ticket_;
    }
    void emit(std::string_view text){if(!hooks_.reply(text))throw std::runtime_error("modem reply queue full");}
    Hooks hooks_;
    State state_=State::command;
    bool carrier_=false;
    uint64_t ticket_=0;
    ModemEscape escape_;
    std::string line_;
    std::map<std::string,int> profile_;
};
}
