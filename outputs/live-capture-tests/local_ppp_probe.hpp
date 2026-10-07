#pragma once
#include "local_discovery_probe.hpp"
#include "local_tcp_probe.hpp"
#include <vector>
#include <cstdint>
#include <iostream>
#include <stdexcept>
// Bounded diagnostic LCP peer, not a full PPP stack. No IP forwarding or authentication.
// RFC 1661/1662. Compression and authentication options are deliberately rejected.
struct LocalPPPProbe {
    using Bytes=std::vector<uint8_t>;
    Bytes frame,out;
    bool started=false,escaped=false,overflow=false,requested=false,localAck=false,peerAck=false,stopped=false;
    unsigned valid=0,invalid=0,tries=0,lastRequest=0;
    bool enableIPCP=false,ipRequested=false,ipLocalAck=false,ipPeerAck=false,ipStopped=false;
    bool enableDiscovery=false,discoverySent=false;
    bool enableTCP=false;
    LocalTCPProbe tcp;
    bool enableSecondPeerTCP=false;
    LocalTCPProbe secondTCP;
    Bytes receiveTCP(const Bytes& ip) {
        auto response=tcp.receive(ip);
        if(enableSecondPeerTCP&&tcp.directPeer&&tcp.closeAcknowledged) {
            // Independent capture-only connection. First capture and stale tuple remain isolated.
            secondTCP.directPeer=true;secondTCP.directGuestPort=1026;
            auto second=secondTCP.receive(ip);
            if(!second.empty()) {
                if(!response.empty())throw std::runtime_error("TCP tuple routing overlap");
                return second;
            }
        }
        return response;
    }
    unsigned ipTries=0,ipLastRequest=0;
    Bytes localIP{3,6,10,0,0,1},guestIP{3,6,10,0,0,2};
    void configureDirectPeerAddresses() {
        if(started || requested || ipRequested || enableDiscovery || enableTCP)
            throw std::runtime_error("direct peer addresses require unused non-service endpoint");
        localIP={3,6,10,0,0,2};guestIP={3,6,10,0,0,1};
    }
    void resetSession() {
        LocalPPPProbe fresh;
        fresh.enableIPCP=enableIPCP;fresh.enableDiscovery=enableDiscovery;fresh.enableTCP=enableTCP;
        fresh.tcp.replyEnd02=tcp.replyEnd02;fresh.tcp.replyState1D=tcp.replyState1D;
        fresh.tcp.mailProbe=tcp.mailProbe;
        fresh.tcp.replyDiagnosticIncomingRecord=tcp.replyDiagnosticIncomingRecord;
        fresh.tcp.diagnosticZeroDebit=tcp.diagnosticZeroDebit;
        fresh.tcp.replyPeerNumber=tcp.replyPeerNumber;
        fresh.tcp.replyReceiverCandidate=tcp.replyReceiverCandidate;
        fresh.tcp.receiverWaitTicks=tcp.receiverWaitTicks;
        fresh.tcp.dateUpdateFixture=tcp.dateUpdateFixture;
        fresh.tcp.diagnosticPeerNumber=tcp.diagnosticPeerNumber;
        fresh.tcp.prepareServiceReply=tcp.prepareServiceReply;
        fresh.tcp.directPeer=tcp.directPeer;
        fresh.tcp.replyPeerLength=tcp.replyPeerLength;
        fresh.tcp.replyPeerProfile=tcp.replyPeerProfile;
        fresh.tcp.replyPeerMarker=tcp.replyPeerMarker;
        fresh.tcp.orderlyClose=tcp.orderlyClose;
        fresh.enableSecondPeerTCP=enableSecondPeerTCP;
        fresh.secondTCP.replyIdleStatus=secondTCP.replyIdleStatus;
        fresh.tcp.directGuestPort=tcp.directGuestPort;
        fresh.localIP=localIP;fresh.guestIP=guestIP;
        *this=std::move(fresh);
    }
    bool open()const{return localAck&&peerAck&&!stopped;}
    bool ipOpen()const{return open()&&ipLocalAck&&ipPeerAck&&!ipStopped;}
    static uint16_t fcs(const Bytes &b){
        uint16_t value=0xffff;
        for(auto c:b){value^=c;for(int i=0;i<8;i++)value=(value&1)?(value>>1)^0x8408:value>>1;}
        return value;
    }
    static Bytes encode(Bytes body){
        auto crc=uint16_t(~fcs(body));body.push_back(uint8_t(crc));body.push_back(uint8_t(crc>>8));
        Bytes wire{0x7e};
        for(auto c:body){if(c<0x20||c==0x7d||c==0x7e){wire.push_back(0x7d);wire.push_back(c^0x20);}else wire.push_back(c);}
        wire.push_back(0x7e);return wire;
    }
    void send(uint8_t code,uint8_t id,const Bytes &options={},uint16_t protocol=0xc021){
        unsigned length=4+unsigned(options.size());
        Bytes body{0xff,3,uint8_t(protocol>>8),uint8_t(protocol),code,id,uint8_t(length>>8),uint8_t(length)};
        body.insert(body.end(),options.begin(),options.end());auto wire=encode(body);
        if(out.size()+wire.size()>8192)throw std::runtime_error("PPP output limit");
        out.insert(out.end(),wire.begin(),wire.end());
        std::cout<<"PPP_TX protocol="<<std::hex<<protocol<<std::dec<<" code="<<unsigned(code)<<" id="<<unsigned(id)<<" length="<<length<<'\n';
    }
    void ipcp(unsigned now){
        if(!enableIPCP||!open()||ipStopped)return;
        if(frame.size()<10){++invalid;return;}
        unsigned length=(frame[6]<<8)|frame[7];
        if(length<4||length>frame.size()-6){++invalid;return;}
        auto code=frame[4],id=frame[5];Bytes options(frame.begin()+8,frame.begin()+4+length);
        if(code==1){
            if(ipOpen()){ipStopped=true;std::cout<<"IPCP_STOP renegotiation outside scope\n";return;}
            Bytes reject;bool hasAddress=false,needsAddress=false;
            for(size_t i=0;i<options.size();){
                if(i+2>options.size()||options[i+1]<2||i+options[i+1]>options.size()){++invalid;return;}
                auto size=options[i+1];
                if(options[i]==3&&size==6){
                    if(hasAddress){++invalid;return;}
                    hasAddress=true;
                    needsAddress=Bytes(options.begin()+i,options.begin()+i+size)!=guestIP;
                }else reject.insert(reject.end(),options.begin()+i,options.begin()+i+size);
                i+=size;
            }
            ipPeerAck=false;
            if(!reject.empty())send(4,id,reject,0x8021);
            else if(!hasAddress||needsAddress)send(3,id,guestIP,0x8021);
            else{send(2,id,options,0x8021);ipPeerAck=true;}
            if(!ipRequested){ipRequested=true;ipTries=1;ipLastRequest=now;send(1,0x61,localIP,0x8021);}
        }else if(code==2&&ipRequested&&id==0x61&&options==localIP)ipLocalAck=true;
        else if((code==3||code==4)&&ipRequested&&id==0x61){ipStopped=true;std::cout<<"IPCP_STOP fixed diagnostic address not accepted\n";}
        else if(code==5){send(6,id,options,0x8021);ipStopped=true;}
        if(ipOpen())std::cout<<"IPCP_OPEN guest=10.0.0."<<unsigned(guestIP[5])<<" peer=10.0.0."<<unsigned(localIP[5])<<"; virtual only, no forwarding\n";
    }
    void packet(unsigned now){
        if(frame.size()<6||fcs(frame)!=0xf0b8||frame[0]!=0xff||frame[1]!=3){++invalid;return;}
        ++valid;
        unsigned protocol=(frame[2]<<8)|frame[3];
        std::cout<<"PPP_RX protocol="<<std::hex<<protocol<<" bytes=";
        for(auto b:frame)std::cout<<unsigned(b)<<',';
        std::cout<<std::dec<<'\n';
        if(protocol==0x8021){ipcp(now);return;}
        if(protocol==0x21 && enableDiscovery && ipOpen() && !discoverySent) {
            auto response=LocalDiscoveryProbe::reply(Bytes(frame.begin()+4,frame.end()-2));
            if(!response.empty()) {
                Bytes body{0xff,3,0,0x21};body.insert(body.end(),response.begin(),response.end());
                auto wire=encode(body);
                if(out.size()+wire.size()>8192)throw std::runtime_error("PPP output limit");
                out.insert(out.end(),wire.begin(),wire.end());discoverySent=true;
                std::cout<<"DISCOVERY_SYNTHETIC endpoint=10.0.0.1:2005; one reply only, no service/authentication\n";
            }
            return;
        }
        if(protocol==0x21 && enableTCP && (discoverySent || tcp.directPeer) && ipOpen()) {
            auto response=receiveTCP(Bytes(frame.begin()+4,frame.end()-2));
            if(!response.empty()) {
                Bytes body{0xff,3,0,0x21};body.insert(body.end(),response.begin(),response.end());
                auto wire=encode(body);
                if(out.size()+wire.size()>8192)throw std::runtime_error("PPP output limit");
                out.insert(out.end(),wire.begin(),wire.end());
            }
            return;
        }
        if(protocol!=0xc021||stopped)return;
        if(frame.size()<10){++invalid;return;}
        unsigned length=(frame[6]<<8)|frame[7];
        if(length<4||length>frame.size()-6){++invalid;return;}
        uint8_t code=frame[4],id=frame[5];
        Bytes options(frame.begin()+8,frame.begin()+4+length);
        if(code==1){
            if(open()){stopped=true;std::cout<<"PPP_STOP renegotiation outside diagnostic scope\n";return;}
            Bytes reject;
            for(size_t i=0;i<options.size();){
                if(i+2>options.size()||options[i+1]<2||i+options[i+1]>options.size()){++invalid;return;}
                auto type=options[i],size=options[i+1];
                bool supported=(type==1&&size==4&&((options[i+2]<<8)|options[i+3])>=128)||(type==5&&size==6);
                if(!supported)reject.insert(reject.end(),options.begin()+i,options.begin()+i+size);
                i+=size;
            }
            peerAck=reject.empty();send(peerAck?2:4,id,peerAck?options:reject);
            if(!requested){requested=true;tries=1;lastRequest=now;send(1,0x51);}
        }else if(code==2&&requested&&id==0x51&&options.empty())localAck=true;
        else if(code==5){send(6,id,options);stopped=true;}
        else if((code==3||code==4)&&id==0x51){stopped=true;std::cout<<"PPP_STOP local defaults not accepted\n";}
        if(open())std::cout<<"PPP_OPEN both LCP directions acknowledged; no NCP or service authentication\n";
    }
    void feed(uint8_t byte,unsigned now){
        if(byte==0x7e){
            if(started&&!frame.empty()){if(escaped||overflow)++invalid;else packet(now);}
            started=true;escaped=overflow=false;frame.clear();return;
        }
        if(!started||overflow)return;
        // Initial receive ACCM=FFFFFFFF; escaped control bytes remain significant.
        if(byte<0x20)return;
        if(escaped){byte^=0x20;escaped=false;}
        else if(byte==0x7d){escaped=true;return;}
        if(frame.size()>=2048){overflow=true;return;}
        frame.push_back(byte);
    }
    void tick(unsigned now){
        if(enableTCP&&ipOpen()){
            auto response=tcp.pollServiceReply();
            if(!response.empty()){
                Bytes body{0xff,3,0,0x21};body.insert(body.end(),response.begin(),response.end());
                auto wire=encode(body);
                if(out.size()+wire.size()>8192)throw std::runtime_error("PPP output limit");
                out.insert(out.end(),wire.begin(),wire.end());
            }
        }
        if(enableIPCP&&open()&&ipRequested&&!ipLocalAck&&!ipStopped&&now-ipLastRequest>=180){
            if(ipTries>=5){ipStopped=true;std::cout<<"IPCP_STOP retry limit\n";}
            else{++ipTries;ipLastRequest=now;send(1,0x61,localIP,0x8021);}
        }
        if(requested&&!localAck&&!stopped&&now-lastRequest>=180){
            if(tries>=5){stopped=true;std::cout<<"PPP_STOP retry limit\n";}
            else{++tries;lastRequest=now;send(1,0x51);}
        }
    }
};
