#pragma once
#include <cstdint>
#include <array>

// Exact observed post-setup packets only. Replies always describe a neutral peer.
// Default: one idle packet. Explicit release experiments permit 256 or 1024.
struct DiagnosticRawTransfer {
    static constexpr std::array<uint8_t,10> packet={0xff,0xff,0xff,0xff,0xff,0xff,0,0,0x8a,0xd0};
    static constexpr std::array<uint8_t,10> pressPacket={0xf7,0xff,0xf7,0xff,0xff,0xff,0,0,0x99,0x48};
    static constexpr std::array<uint8_t,10> holdPacket={0xf7,0xff,0xff,0xff,0xff,0xff,0,0,0x94,0x0a};
    static constexpr std::array<uint8_t,10> releasePacket={0xff,0xff,0xff,0xff,0xf7,0xff,0,0,0x0f,0x13};
    bool armed=false,halted=false;
    bool neutralInputs=false;
    bool releaseInputs=false;
    unsigned candidates=1,pressPackets=0,holdPackets=0,releasePackets=0;
    unsigned received=0,sent=0,limit=1;
    void arm(unsigned maxPackets=1,bool allowStart=false,bool allowRelease=false){
        if(!armed&&!halted){releaseInputs=allowStart&&allowRelease;limit=(maxPackets<=32||(releaseInputs&&(maxPackets==256||maxPackets==1024)))?maxPackets:0;neutralInputs=allowStart;candidates=releaseInputs?15:allowStart?7:1;armed=true;}
    }
    void stop(){halted=true;}
    void feed(uint8_t b){
        if(!armed||halted||received==packet.size()*limit)return;
        const auto index=received%packet.size();
        if(b!=packet[index])candidates&=~1u;
        if(b!=pressPacket[index])candidates&=~2u;
        if(b!=holdPacket[index])candidates&=~4u;
        if(b!=releasePacket[index])candidates&=~8u;
        if(!candidates){stop();return;}
        ++received;
        if(received%packet.size()==0){if(candidates==2)++pressPackets;if(candidates==4)++holdPackets;if(candidates==8)++releasePackets;candidates=releaseInputs?15:neutralInputs?7:1;}
    }
    bool peek(uint8_t &b) const{
        if(!armed||halted||sent>=(received/packet.size())*packet.size())return false;
        b=packet[sent%packet.size()];return true;
    }
    void commit(){uint8_t b;if(peek(b))++sent;}
};

// Only the observed calibration packet; maximum 16 packets in one guarded phase.
struct DiagnosticRawCalibration {
    static constexpr std::array<uint8_t,10> packet={1,1,1,1,1,1,1,1,0x43,0xe9};
    bool armed=false,halted=false,ready=false;
    unsigned received=0,sent=0,replies=0;
    void arm() {if(!armed&&!halted)armed=true;}
    void stop() {halted=true;ready=false;}
    void feed(uint8_t b) {
        if(!armed||halted)return;
        if(ready||replies>=16||b!=packet[received]) {stop();return;}
        ready=++received==packet.size();
    }
    bool peek(uint8_t &b) const {
        if(!armed||halted||!ready)return false;
        b=packet[sent];return true;
    }
    void commit() {
        uint8_t b;if(!peek(b))return;
        if(++sent==packet.size()) {++replies;received=sent=0;ready=false;}
    }
};

// Bounded synthetic a..f responder, NOT a general modem echo or game peer.
struct DiagnosticRawHandshake {
    bool started=false, halted=false, complete=false, pending=false;
    uint8_t expected=0x61, last=0;
    unsigned replies=0;
    void start() { if(!started)started=true; }
    void stop() { halted=true;pending=false; }
    void feed(uint8_t b) {
        if(!started||halted||complete)return;
        if(b==expected)pending=true;
        else if(b!=last)stop(); // Only duplicates of the preceding token are tolerated.
    }
    bool peek(uint8_t &b) const {
        if(!started||halted||complete||!pending||replies>=6)return false;
        b=expected;return true;
    }
    void sent() {
        uint8_t b;
        if(!peek(b))return;
        last=expected++;pending=false;++replies;
        complete=replies==6;
    }
};

// One observed six-byte parameter packet plus CRC. No general echo/retry.
struct DiagnosticRawParameters {
    static constexpr std::array<uint8_t,8> packet={1,0,8,1,0,0,0xf9,0x43};
    static constexpr std::array<uint8_t,8> secondPacket={2,2,8,1,0,0,0x73,0x20};
    const std::array<uint8_t,8> expectedPacket;
    explicit DiagnosticRawParameters(bool second=false):expectedPacket(second?secondPacket:packet){}
    bool armed=false,halted=false,ready=false;
    unsigned received=0,sent=0;
    void arm() { if(!halted)armed=true; }
    void stop() { halted=true;ready=false; }
    void feed(uint8_t b) {
        if(!armed||halted||received==packet.size())return;
        if(b!=expectedPacket[received]) {stop();return;}
        ready=++received==packet.size();
    }
    bool peek(uint8_t &b) const {
        if(!armed||halted||!ready||sent==packet.size())return false;
        b=expectedPacket[sent];return true;
    }
    void commit() { uint8_t b;if(peek(b))++sent; }
};
