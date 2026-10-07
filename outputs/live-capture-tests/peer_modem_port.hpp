#pragma once
#include "diagnostic_modem_route.hpp"
#include "diagnostic_modem_profile.hpp"
#include "diagnostic_escape.hpp"
#include "diagnostic_phone_directory.hpp"
#include <string>
#include <deque>
// Byte-level adapter for future UART THR/RBR hooks. No real dialing, baud timing,
// register/IRQ emulation or login. Escape timing is the existing experimental
// 60-emulated-frame guard, not a verified hardware timing model.
class PeerModemPort {
public:
    using Phase=DiagnosticModemRoute::Phase;
    struct Snapshot {
        DiagnosticModemRoute::Snapshot route;
        bool dataMode; // Last applied AT state, not a claim that carrier is live.
        size_t pendingResponses,pendingTransmit;
    };
    // Does not emit RING/CONNECT/NO CARRIER, tick the clock, or drain data.
    Snapshot snapshot() const {
        return {route.snapshot(),data,responses.size(),escape.out.size()};
    }
    PeerModemPort(PeerLine &line,unsigned side):route(line,side){}
    PeerModemPort(PeerLine &line,unsigned side,const DiagnosticPhoneDirectory &subscribers):route(line,side),directory(&subscribers),subscriberSide(side){}
    PeerModemPort(PeerLine &,unsigned,DiagnosticPhoneDirectory &&)=delete;
    void adoptAnsweredSession(uint64_t token) {route.adoptAnsweredSession(token);poll();}
    void poll() {
        auto now=route.phase();
        if(now==last)return;
        if(now==Phase::Incoming)reply("\r\nRING\r\n");
        if(now==Phase::Peer){data=true;escape.reset(frame);reply("\r\nCONNECT 14400\r\n");}
        if(now==Phase::Idle && last!=Phase::Idle){
            data=false;escape.reset(frame);overflow=false;command.clear();responses.clear();reply("\r\nNO CARRIER\r\n");
        }
        last=now;
    }
    bool transmit(uint8_t byte) {
        tick(frame);
        if(data) {
            // Retained bytes must drain before accepting another input byte.
            // Once accepted, an input is never returned as rejected/retryable.
            if(!escape.out.empty())return false;
            escape.feed(byte,frame);
            flushData();
            return true;
        }
        if(byte=='\n')return true;
        if(byte!='\r') {
            if(command.size()==128){command.clear();overflow=true;}
            if(!overflow)command.push_back(char(byte>='a'&&byte<='z'?byte-'a'+'A':byte));
            return true;
        }
        if(overflow){overflow=false;command.clear();reply("\r\nERROR\r\n");return true;}
        auto cmd=command;command.clear();
        if(cmd=="AT")reply("\r\nOK\r\n");
        else if(cmd=="ATH0") {reset();reply("\r\nOK\r\n");}
        else if(cmd=="ATZ" || cmd=="ATZ0") {reset();settings.clear();reply("\r\nOK\r\n");}
        else if(applyDiagnosticModemProfile(cmd,true,settings))reply("\r\nOK\r\n");
        else if(cmd=="ATO" || cmd=="ATO0") {
            if(route.phase()==Phase::Peer){data=true;escape.reset(frame);reply("\r\nCONNECT 14400\r\n");}
            else reply("\r\nNO CARRIER\r\n");
        }
        else if(cmd=="ATA") {if(!route.answerPeer())reply("\r\nNO CARRIER\r\n");poll();}
        else if(directory&&(cmd.starts_with("ATDT")||cmd.starts_with("ATS91=15S92=15DT"))) {
            auto number=cmd.substr(cmd.starts_with("ATDT")?4:16);
            if(number.starts_with(' '))number.erase(0,1);
            const auto destination=directory->resolve(subscriberSide,number);
            if(destination==DiagnosticPhoneDirectory::Destination::Self)reply("\r\nBUSY\r\n");
            else if(destination==DiagnosticPhoneDirectory::Destination::Unknown)reply("\r\nNO CARRIER\r\n");
            else {if(!route.dialPeer())reply("\r\nBUSY\r\n");poll();}
        }
        else if(!directory&&(cmd=="ATDT5550100" || cmd=="ATDT 5550100" || cmd=="ATS91=15S92=15DT5550100")) {
            if(!route.dialPeer())reply("\r\nBUSY\r\n");
            poll(); // No CONNECT until the other adapter processes ATA.
        } else reply("\r\nERROR\r\n");
        return true;
    }
    bool receive(uint8_t &byte) {
        tick(frame);
        if(!responses.empty()){byte=responses.front();responses.pop_front();return true;}
        return data && route.peerReceive(byte);
    }
    // DCD remains asserted in online command mode after +++.
    bool carrier() {poll();return route.phase()==Phase::Peer;}
    void tick(unsigned now) {
        frame=now;poll();
        if(!data)return;
        escape.tick(frame);
        flushData();
        if(escape.escaped && escape.out.empty()) {
            data=false;command.clear();reply("\r\nOK\r\n");
        }
    }
    const std::map<std::string,int> &profile() const {return settings;}
    void reset() {
        route.hangup();last=Phase::Idle;data=false;overflow=false;
        command.clear();responses.clear();escape.reset(frame);
    }
private:
    DiagnosticModemRoute route;
    const DiagnosticPhoneDirectory *directory=nullptr; // Owner outlives this port; immutable configuration.
    unsigned subscriberSide=0;
    Phase last=Phase::Idle;
    bool data=false,overflow=false;
    unsigned frame=0;
    DiagnosticEscape escape;
    std::string command;
    std::map<std::string,int> settings;
    std::deque<uint8_t> responses;
    void flushData() {
        size_t sent=0;
        while(sent<escape.out.size() && route.peerSend(escape.out[sent])==PeerLine::Send::Delivered)++sent;
        escape.out.erase(escape.out.begin(),escape.out.begin()+sent);
    }
    void reply(const std::string &s) {
        if(responses.size()+s.size()>256)throw std::runtime_error("modem response overflow");
        for(unsigned char b:s)responses.push_back(b);
    }
};
