#pragma once
#include "peer_modem_port.hpp"
#include <chrono>
#include <ostream>
// Diagnostic service phases, NOT authentication or account states.
inline unsigned servicePhase(const LocalPPPProbe &p,bool connected,bool peer,
                             bool helo,bool login,bool password) {
    if(peer)return 10;
    if(!connected)return 0;
    if(p.stopped||p.ipStopped||p.tcp.state==LocalTCPProbe::State::Stopped)return 11;
    if(p.tcp.end02Sent)return 9;
    if(!p.tcp.captured.empty())return 8;
    if(p.tcp.state==LocalTCPProbe::State::Established)return 7;
    if(p.ipOpen())return 6;
    if(p.open())return 5;
    if(password)return 4;
    if(login)return 3;
    if(helo)return 2;
    return 1;
}
inline void writeServiceStatus(std::ostream &out,unsigned side,const LocalPPPProbe &p,
                               bool connected,bool peer,bool helo,bool login,bool password,
                               const std::string &subscriber="") {
    // Only canonical local directory numbers; preserve leading zeroes as text.
    if(subscriber.size()>20 || subscriber.find_first_not_of("0123456789")!=std::string::npos)
        throw std::invalid_argument("invalid status subscriber");
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out<<"SERVICE_STATUS {\"version\":1,\"time_ms\":"<<std::dec<<ms
       <<",\"side\":"<<side<<",\"phase\":"<<servicePhase(p,connected,peer,helo,login,password)
       <<",\"captured\":"<<p.tcp.captured.size()<<",\"invalid\":"<<p.invalid
       <<",\"subscriber\":\""<<subscriber<<"\"}\n";
}
// Fixed numeric JSON only; no phone numbers or guest payload. Owner-thread use.
inline void writeModemStatus(std::ostream &out,const PeerModemPort &port) {
    const auto s=port.snapshot();
    const auto &r=s.route;const auto &w=r.line;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out<<"MODEM_STATUS {\"version\":1,\"time_ms\":"<<std::dec<<ms
       <<",\"side\":"<<r.endpoint<<",\"phase\":"<<int(r.phase)
       <<",\"session\":"<<w.session<<",\"sent\":"<<w.sent[r.endpoint]
       <<",\"received\":"<<w.received[r.endpoint]
       <<",\"pending\":"<<w.pendingReceive[r.endpoint]<<"}\n";
}
