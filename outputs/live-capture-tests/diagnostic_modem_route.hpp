#pragma once
#include "peer_line.hpp"
#include "local_ppp_probe.hpp"
#include <memory>
// Routing layer only. A future UART/AT adapter must translate state transitions
// into modem responses. Service mode begins AFTER the login prompt exchange.
class DiagnosticModemRoute {
public:
    enum class Phase {Idle, ServicePPP, Dialing, Incoming, Peer};
    struct Snapshot {
        unsigned endpoint;
        Phase phase;
        uint64_t serviceSession;
        size_t pendingServiceReply;
        PeerLine::Snapshot line;
    };
    // A passive view: unlike phase(), never polls or changes routing state.
    // Must be sampled on the emulation/owner thread, then copied to the UI.
    Snapshot snapshot() const {
        auto view=current;
        if(view!=Phase::ServicePPP) {
            if(view==Phase::Idle) {
                if(line.ringing(side))view=Phase::Incoming;
            } else if(peerToken!=line.session() || line.state()==PeerLine::State::Idle) {
                view=Phase::Idle;
            } else if(line.state()==PeerLine::State::Connected) {
                view=Phase::Peer;
            }
        }
        return {side,view,serviceGeneration,service?service->out.size():0,line.snapshot()};
    }
    DiagnosticModemRoute(PeerLine &wire,unsigned endpoint):line(wire),side(endpoint) {
        if(side>1)throw std::out_of_range("modem endpoint");
    }
    DiagnosticModemRoute(const DiagnosticModemRoute&)=delete;
    DiagnosticModemRoute& operator=(const DiagnosticModemRoute&)=delete;
    ~DiagnosticModemRoute(){hangup();}
    Phase phase() {poll();return current;}
    uint64_t serviceSession() const {return serviceGeneration;}
    void adoptAnsweredSession(uint64_t token) {
        if(current!=Phase::Idle||service||line.state()!=PeerLine::State::Connected||token!=line.session())
            throw std::runtime_error("answered-session handoff requires current connected control session");
        peerToken=token;current=Phase::Peer;
    }
    bool startService() {
        if(phase()!=Phase::Idle || line.state()!=PeerLine::State::Idle)return false;
        service=std::make_unique<LocalPPPProbe>();++serviceGeneration;
        current=Phase::ServicePPP;return true;
    }
    bool serviceByte(uint64_t token,uint8_t byte,unsigned frame) {
        if(current!=Phase::ServicePPP || token!=serviceGeneration)return false;
        service->feed(byte,frame);return true;
    }
    bool serviceReceive(uint64_t token,uint8_t &byte) {
        if(current!=Phase::ServicePPP || token!=serviceGeneration || service->out.empty())return false;
        byte=service->out.front();service->out.erase(service->out.begin());return true;
    }
    const LocalPPPProbe *serviceState() const {return service.get();}
    bool dialPeer() {
        if(phase()!=Phase::Idle || !line.dial(side))return false;
        peerToken=line.session();current=Phase::Dialing;return true;
    }
    void poll() {
        if(current==Phase::ServicePPP)return;
        if(current==Phase::Idle) {
            if(line.ringing(side)){peerToken=line.session();current=Phase::Incoming;}
            return;
        }
        if(peerToken!=line.session() || line.state()==PeerLine::State::Idle)current=Phase::Idle;
        else if(line.state()==PeerLine::State::Connected)current=Phase::Peer;
    }
    bool answerPeer() {
        poll();
        if(current!=Phase::Incoming || !line.answer(side,peerToken))return false;
        current=Phase::Peer;return true;
    }
    PeerLine::Send peerSend(uint8_t byte) {
        poll();if(current!=Phase::Peer)return PeerLine::Send::NoCarrier;
        return line.send(side,peerToken,byte);
    }
    bool peerReceive(uint8_t &byte) {poll();return current==Phase::Peer && line.receive(side,peerToken,byte);}
    void hangup() {
        if(current==Phase::Dialing || current==Phase::Incoming || current==Phase::Peer)line.hangup(side,peerToken);
        // Drop the whole PPP object: parser fragments, replies and TCP state must
        // never be delivered to a subsequent direct peer session.
        service.reset();current=Phase::Idle;
    }
private:
    PeerLine &line;
    unsigned side;
    uint64_t peerToken=0,serviceGeneration=0;
    Phase current=Phase::Idle;
    std::unique_ptr<LocalPPPProbe> service;
};
