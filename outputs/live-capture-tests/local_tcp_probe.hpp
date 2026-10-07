#pragma once
#include "local_discovery_probe.hpp"
#include "diagnostic_peer_profile.hpp"
#include "diagnostic_incoming_record.hpp"
#include "diagnostic_outgoing_record.hpp"
#include "diagnostic_postmatch_request.hpp"
#include "diagnostic_date_fixture.hpp"
#include "diagnostic_level_fixture.hpp"
#include "../../components/xband/include/xband/local_phone_policy.hpp"
#include <iostream>
#include <stdexcept>
#include <functional>
// One fixed virtual connection; optional fixed diagnostic reply. Not a general TCP stack.
struct LocalTCPProbe {
    using Bytes=LocalDiscoveryProbe::Bytes;
    using D=LocalDiscoveryProbe;
    enum class State { Listen, SynReceived, Established, Stopped, LastAck };
    State state=State::Listen;
    uint32_t guestInitial=0,nextGuest=0;
    static constexpr uint32_t localInitial=0x10203040;
    Bytes captured;
    bool directPeer=false; // Fixed .1:1025 -> .2:3000 capture-only endpoint.
    uint16_t directGuestPort=1025;
    uint16_t serviceGuestPort=1024;
    bool replyIdleStatus=false,statusPending=false,statusHalted=false;
    unsigned statusReplies=0;
    bool replyPeerLength=false,peerLengthSent=false,peerLengthAcknowledged=false;
    bool replyPeerProfile=false;
    bool replyPeerMarker=false,peerMarkerSent=false;
    bool orderlyClose=false,closeAcknowledged=false,postCloseLogged=false;
    uint32_t finSequence=0;
    Bytes peerProfile;
    size_t peerProfileSent=0;
    bool replyEnd02=false,end02Sent=false,end02Acknowledged=false;
    bool replyPostMatchEnd02=false; // Explicit isolated VF post-call experiment only.
    bool mailProbe=false; // Explicit package-only synthetic service terminator.
    bool profileMailExperiment=false; // Fresh-COW capture-only player 0/3 experiment, OFF.
    bool replyDiagnosticIncomingRecord=false; // Fixed opt-in mail-only fixture, default OFF.
    Bytes diagnosticMailboxReply; // Opt-in bounded encoder output, not raw guest code.
    Bytes matchMailPrefix; // Journal mail/clear commands, without exchange terminator.
    Bytes rankingResultReply; // Opt-in server ledger update, command25 only.
    bool replyState1D=false;
    bool diagnosticZeroDebit=false; // Explicit synthetic all-zero card test only.
    bool replyPeerNumber=false; // Fixed diagnostic sentinel, never a real dial target.
    bool replyReceiverCandidate=false; // Service-only 1C/0E hypothesis, NOT a verified receiving role.
    uint32_t receiverWaitTicks=0; // Diagnostic guest-clock units, not host milliseconds.
    Bytes standbyDialogReply; // Original command22, opt-in receiver-only notice.
    std::optional<diagnostic::DateFixture> dateUpdateFixture; // OFF unless explicit isolated trial.
    bool levelDisplayFixture=false; // Explicit synthetic ranking, no win/loss changes.
    std::function<Bytes(std::optional<uint32_t>)> gameRankingReply;
    std::function<Bytes(std::optional<uint32_t>)> gameIntroTitleReply;
    std::function<Bytes(std::optional<uint32_t>)> gameMatchAwardReply;
    std::function<Bytes(const std::string&,std::optional<uint32_t>)> usageAreaReply;
    std::function<Bytes(const std::string&,std::optional<uint32_t>,std::optional<uint8_t>)> usageAreaPreferenceReply;
    std::function<Bytes(const std::string&,std::optional<uint32_t>)> regionTownReply;
    std::string diagnosticPeerNumber="5550100";
    std::function<bool()> prepareServiceReply;
    std::function<void()> onServiceReplySent;
    unsigned serviceWindow=0;
    Bytes pollServiceReply(){
        if(directPeer||!replyEnd02||end02Sent||state!=State::Established||
           !(completeMailProbeRequest(captured,profileMailExperiment) ||
             (replyPostMatchEnd02 && diagnostic::observedPostMatchRequest(captured))))return {};
        if(prepareServiceReply&&!prepareServiceReply())return {};
        const Bytes response=applicationReply();
        if(serviceWindow<response.size())return {};
        const auto seqOut=localNext;
        const auto packet=reply(seqOut,nextGuest,0x18,response);
        end02Sent=true;localNext+=uint32_t(response.size());
        if(onServiceReplySent)onServiceReplySent();
        std::cout<<"TCP_SYNTHETIC_REPLY commands="<<(!matchMailPrefix.empty()?"journal mail prefix,":"")<<(dateUpdateFixture&&response.front()==4?"04,":"")<<(!diagnosticMailboxReply.empty()?"bounded mail response (see experiment logs)":replyDiagnosticIncomingRecord?"1E,02 (synthetic record)":diagnosticZeroDebit?"49,02 (synthetic zero-card debit)":replyReceiverCandidate?"1C,0E,02":replyState1D?"1D,02":"02")<<" bytes="<<response.size()<<"; diagnostic only, NOT authentication\n";
        return packet;
    }
    static bool completeDiagnosticRequest(const Bytes &b) {
        // Observed PB3 setup fixture only, NOT a general XBAND parser/auth check.
        // Card block: 1E,00,result16,length32,raw-card bytes. Its offset and
        // total record size move with the length-prefixed phone field.
        // Checking the full size prevents replies to fragmented requests.
        if(b.size()<143 || Bytes(b.begin(),b.begin()+6)!=Bytes{0x1f,0x74,0x6a,0x30,0x34,0x0b})return false;
        if(b[22]<2 || b[22]>33)return false;
        const auto words=xband::registrationOffset(b,115);
        if(words+2>b.size()||D::word(b,words)>16)return false;
        const auto card=xband::registrationOffset(b,135);
        if(b.size()<card+8 || b[card]!=0x1e || b[card+1]!=0)return false;
        // Validate the established tags after the bounded registration list,
        // rather than searching for a card tag inside arbitrary resource data.
        if(b[xband::registrationOffset(b,121)]!=6 ||
           b[xband::registrationOffset(b,128)]!=0x0b)return false;
        const auto cardLength=longword(b,card+4);
        if(cardLength!=0 && cardLength!=13)return false;
        if(b.size()>8192)return false;
        size_t pos=xband::registrationOffset(b,184)+cardLength;
        auto available=[&](size_t n){return pos<=b.size()&&n<=b.size()-pos;};
        if(!available(3)||b[pos]!=0x15)return false;
        const auto count=D::word(b,pos+1);pos+=3;
        if(count>16)return false;
        for(unsigned i=0;i<count;++i){
            if(!available(6))return false;
            const auto type=D::word(b,pos),length=longword(b,pos+2);pos+=6;
            if((type!=1&&type!=0x0201)||!available(length))return false;
            pos+=length;
        }
        // Only the observed empty 16/1D lists are supported. Unknown layouts
        // must not be mistaken for a completed matchmaking request.
        if(!available(6)||Bytes(b.begin()+pos,b.begin()+pos+6)!=Bytes{0x16,0,0,0x1d,0,0})return false;
        pos+=6;if(!available(1))return false;
        if(b[pos]==0x21){++pos;}
        else if(b[pos]==0x20){
            if(!available(5))return false;
            const auto length=longword(b,pos+1);pos+=5;
            if(!available(length))return false;pos+=length;
            if(!available(4))return false;pos+=4; // opaque trailing scalar, not interpreted as a result
        }else return false;
        const Bytes tail{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0};
        return available(tail.size())&&pos+tail.size()==b.size()&&std::equal(tail.begin(),tail.end(),b.begin()+pos);
    }
    // VF REMIX mail request observed after a match (2026-10-04): one rival
    // entry precedes the outgoing 1D list, followed by an opaque 20 block.
    // Subsequent match/mail requests add observed 0200 (12 bytes), 0201
    // and 0202 (324 bytes each) resources. Skip their bounded framing;
    // their contents are opaque and are not interpreted as mail or scores.
    // Only framing is interpreted here; neither the rival entry nor the 20
    // block is accepted as a score, receipt, or authenticated identity.
    static bool rivalMailListBounds(const Bytes &b,size_t &listStart,size_t &listEnd) {
        try {
            if(b.size()<600||b.size()>8192||
               b.size()<6||Bytes(b.begin(),b.begin()+6)!=Bytes{0x1f,0x74,0x6a,0x30,0x34,0x0b})return false;
            xband::registrationPhone(b);
            const size_t card=xband::registrationOffset(b,135);
            if(card+8>b.size()||b[card]!=0x1e||b[card+1]!=0||longword(b,card+4)!=0)return false;
            size_t pos=xband::registrationOffset(b,184);
            auto available=[&](size_t n){return pos<=b.size()&&n<=b.size()-pos;};
            if(!available(3)||b[pos]!=0x15)return false;
            const auto resources=D::word(b,pos+1);pos+=3;
            if(resources<1||resources>4)return false;
            unsigned seenResources=0;
            for(unsigned i=0;i<resources;++i){
                if(!available(6))return false;
                const auto type=D::word(b,pos),length=longword(b,pos+2);pos+=6;
                unsigned bit=0;
                if(i==0&&type==1&&length==276)bit=1;
                else if(i>0&&type==0x0200&&length==12)bit=2;
                else if(i>0&&type==0x0201&&length==324)bit=4;
                else if(i>0&&type==0x0202&&length==324)bit=8;
                if(!bit||(seenResources&bit)||!available(length))return false;
                seenResources|=bit;
                pos+=length;
            }
            if(!available(3)||b[pos]!=0x16)return false;
            const auto rivalCount=D::word(b,pos+1);
            // A switched profile can have no rivals: captured profile2/name21
            // emits 16 0000 before its existing shared outgoing-mail list.
            // Zero and FFFF both carry no entry; neither permits partial data.
            pos+=3;
            // Rival-list size is independent of the four local profile slots.
            // Bound iterations by available bytes: every framed rival needs
            // 12 prefix bytes and at least a two-byte terminated name.
            // Preserve the absent-list FFFF sentinel; reject partial records.
            if(rivalCount!=0xffff&&rivalCount>(b.size()-pos)/14)return false;
            // Observed absent-rival framing: 16 FF FF has no entry.
            // This sentinel is not a count of 65535 records.
            for(unsigned rival=0;rivalCount!=0xffff&&rival<rivalCount;++rival){
            // Original0602D1FC emits kind:u16(00FF), serial[8], player:u8,
            // then nameLength:u8 when the serial ends in FFFFFFFF.
            // The previous parser accidentally treated player+length as u16;
            // this worked only for player0 and rejected the ROM's FF player.
            if(!available(12)||D::word(b,pos)!=0x00ff||
               !std::all_of(b.begin()+pos+2,b.begin()+pos+10,[](uint8_t v){return v==0xff;})||
               (b[pos+10]>3&&b[pos+10]!=0xff))return false;
            pos+=11;
            const auto nameLength=b[pos++];
            if(nameLength<2||nameLength>32||!available(nameLength)||b[pos+nameLength-1]!=0||
               std::find(b.begin()+pos,b.begin()+pos+nameLength-1,0)!=b.begin()+pos+nameLength-1)return false;
            pos+=nameLength;
            }
            listStart=pos;
            if(!available(3)||b[pos]!=0x1d)return false;
            const auto count=D::word(b,pos+1);pos+=3;
            // Each complete record needs at least 10 prefix + 6 lengths +
            // 3 nonempty field bytes + 4 suffix bytes. Bound iterations by
            // the request, not the number of users or a captured mail count.
            if(count>(b.size()-pos)/23)return false;
            for(unsigned i=0;i<count;++i){
                // Four local profile slots share the submitted outbox list.
                // This validates framing only, not source identity/custody.
                // The captured mail-only request carries players 0 and 2.
                if(!available(10)||b[pos]>3||
                   !std::all_of(b.begin()+pos+1,b.begin()+pos+10,[](uint8_t v){return v==0xff;}))return false;
                pos+=10;
                for(unsigned field=0;field<3;++field){
                    if(!available(2))return false;
                    const auto length=D::word(b,pos);pos+=2;
                    const auto limit=field==2?diagnostic::maxObservedOutgoingBodyBytes:diagnostic::maxObservedOutgoingTextBytes;
                    if(!length||length>limit||!available(length))return false;
                    pos+=length;
                }
                if(!available(4)||!std::all_of(b.begin()+pos,b.begin()+pos+4,[](uint8_t v){return v==0;}))return false;
                pos+=4;
            }
            listEnd=pos;
            // Observed after the paused/ended VF call: 21 then 23 with
            // scalar 84 and exactly 80 opaque bytes, followed by 26/26/1B/29.
            // Do not interpret the scalar or opaque bytes as a result/receipt.
            if(available(6)&&b[pos]==0x21&&b[pos+1]==0x23&&longword(b,pos+2)==84){
                pos+=6;
                if(!available(80))return false;
                pos+=80;
                constexpr std::array<uint8_t,12> alternateTail{0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0};
                return pos+alternateTail.size()==b.size()&&
                    std::equal(alternateTail.begin(),alternateTail.end(),b.begin()+pos);
            }
            if(!available(5)||b[pos]!=0x20||longword(b,pos+1)!=84)return false;
            pos+=5;
            if(!available(84+4+13))return false;
            pos+=84+4; // Opaque result block and scalar: not a mail acknowledgement.
            constexpr std::array<uint8_t,4> tailStart{0x24,0x26,0x26,0x1b};
            if(!available(8)||!std::equal(tailStart.begin(),tailStart.end(),b.begin()+pos))return false;
            pos+=4;
            if(longword(b,pos)==1){pos+=4;}
            else if(longword(b,pos)==0x11||longword(b,pos)==0x19){
                // Observed 0x19 suffix carries two bounded NUL-terminated
                // texts (22 and 25 bytes). Scalar semantics remain unknown.
                const bool twoTexts=longword(b,pos)==0x19;
                pos+=4;
                if(twoTexts){
                    if(!available(2)||D::word(b,pos)!=22)return false;
                    pos+=2;
                    if(!available(22)||b[pos+21]!=0)return false;
                    pos+=22;
                }
                if(!available(2))return false;
                const auto length=D::word(b,pos);pos+=2;
                if(length!=25||!available(length)||b[pos+length-1]!=0)return false;
                pos+=length;
            }else return false;
            constexpr std::array<uint8_t,5> tailEnd{0x29,0,0,0,0};
            return pos+tailEnd.size()==b.size()&&std::equal(tailEnd.begin(),tailEnd.end(),b.begin()+pos);
        }catch(const std::exception&){return false;}
    }
    // Original0602CAE4: selector02 has nameLength:u8 and NUL-terminated name.
    // Strip only this bounded header extension for existing body validation.
    // The actual selector/target are retained for routing; never authentication.
    static std::optional<std::pair<Bytes,std::string>> namedRequestBody(const Bytes &b) {
        try {
            const auto code=xband::registrationOffset(b,160);
            if(code<1||code+2>b.size()||b[code-1]!=0x0e||b[code]!=2)return {};
            const auto n=b[code+1];
            if(n<2||n>32||code+2+n>b.size()||b[code+1+n]!=0||
               std::find(b.begin()+code+2,b.begin()+code+1+n,0)!=b.begin()+code+1+n)return {};
            std::string target(b.begin()+code+2,b.begin()+code+1+n);
            auto normalized=b;normalized.erase(normalized.begin()+code+1,normalized.begin()+code+2+n);
            normalized[code]=3;
            return std::pair{std::move(normalized),std::move(target)};
        }catch(const std::exception&){return {};}
    }
    static bool completeMailProbeRequest(const Bytes &b,bool allowObservedPlayer3=false) {
        if(const auto named=namedRequestBody(b))return completeMailProbeRequest(named->first,allowObservedPlayer3);
        size_t rivalListStart=0,rivalListEnd=0;
        if(rivalMailListBounds(b,rivalListStart,rivalListEnd))return true;
        if(completeDiagnosticRequest(b))return true;
        // Locate the observed player-0 list structurally; do not search body
        // bytes for 1D/21. Keep auxiliary semantics in the custody decoder so
        // unsupported suffix data cannot be retained or authorize clear.
        constexpr size_t tailSize=13;
        if(b.size()<tailSize+1+3+22||b.size()>8192)return false;
        const size_t marker=b.size()-tailSize-1;
        if(b[marker]!=0x21)return false;
        if(b.size()<143||b[22]<2||b[22]>33)return false;
        const auto words=xband::registrationOffset(b,115);
        if(words+2>b.size()||D::word(b,words)>16)return false;
        const auto card=xband::registrationOffset(b,135);
        if(card+8>b.size())return false;
        const auto cardLength=longword(b,card+4);
        if(cardLength!=0&&cardLength!=13)return false;
        size_t pos=xband::registrationOffset(b,184)+cardLength;
        auto available=[&](size_t n){return pos<=marker&&n<=marker-pos;};
        if(!available(3)||b[pos]!=0x15)return false;
        const auto resources=D::word(b,pos+1);pos+=3;
        if(resources>16)return false;
        for(unsigned i=0;i<resources;++i){
            if(!available(6))return false;
            const auto length=longword(b,pos+2);pos+=6;
            if(!available(length))return false;pos+=length;
        }
        if(!available(6)||b[pos]!=0x16||b[pos+1]!=0||b[pos+2]!=0)return false;
        pos+=3;const auto tag=pos;
        if(b[pos]!=0x1d)return false;
        const auto count=D::word(b,pos+1);pos+=3;
        if(!count||count>(marker-pos)/23)return false;
        for(unsigned i=0;i<count;++i){
            if(!available(10))return false;
            const auto player=b[pos++];
            if(player>3||
               !std::all_of(b.begin()+pos,b.begin()+pos+9,[](uint8_t v){return v==0xff;}))return false;
            pos+=9;
            for(unsigned field=0;field<3;++field){
                if(!available(2))return false;
                const auto length=D::word(b,pos);pos+=2;
                const auto limit=field==2?diagnostic::maxObservedOutgoingBodyBytes:diagnostic::maxObservedOutgoingTextBytes;
                if(!length||length>limit||!available(length))return false;
                pos+=length;
            }
            if(!available(4)||!std::all_of(b.begin()+pos,b.begin()+pos+4,[](uint8_t v){return v==0;}))return false;
            pos+=4;
        }
        if(pos!=marker)return false;
        Bytes normalized=b;normalized[tag+1]=normalized[tag+2]=0;
        normalized.erase(normalized.begin()+tag+3,normalized.begin()+marker);
        return completeDiagnosticRequest(normalized);
    }
    static Bytes observedGameResult(const Bytes &b,bool includeErrors=false) {
        if(const auto named=namedRequestBody(b))return observedGameResult(named->first,includeErrors);
        // Only structurally validated complete requests. Never search profile
        // or mail bodies for a result-looking byte pattern.
        size_t listStart=0,pos=0;
        if(!rivalMailListBounds(b,listStart,pos)){
            if(!completeDiagnosticRequest(b))return {};
            const auto card=xband::registrationOffset(b,135);
            pos=xband::registrationOffset(b,184)+longword(b,card+4);
            const auto count=D::word(b,pos+1);pos+=3;
            for(unsigned i=0;i<count;++i){const auto n=longword(b,pos+2);pos+=6+n;}
            pos+=6; // validated empty 16/1D lists
        }
        if(pos+85>b.size()||b[pos]!=0x20||longword(b,pos+1)!=84)return {};
        Bytes result(b.begin()+pos+1,b.begin()+pos+85);
        if(!includeErrors&&longword(result,8)!=0)return {};
        return result;
    }
    static Bytes observedVFResult(const Bytes &b) {
        auto result=observedGameResult(b);
        if(result.empty()||longword(result,4)!=0x00010003)return {};
        return result;
    }
    // Observed outgoing service selector immediately follows the 0E tag.
    // 03 requests a match, 04 requests mail-only access on the same TCP route.
    // The registration phone field shifts this offset; no fixed absolute byte.
    static uint8_t serviceRequestCode(const Bytes &b,bool allowObservedPlayer3=false) {
        if(!completeMailProbeRequest(b,allowObservedPlayer3))return 0;
        const size_t code=xband::registrationOffset(b,160);
        if(code==0||code>=b.size()||b[code-1]!=0x0e)return 0;
        if(b[code]==2)return namedRequestBody(b)?2:0;
        return b[code]==3||b[code]==4?b[code]:0;
    }
    // Original 0602C81C registration serializer: command0B header flags
    // at application offset14. 0602C8C0..C8D8 ORs bit0 when selected-user
    // getter00280017 returns variable25 bit(user+4). Resource0200:0021
    // orders choices as yes=0, no=1. This is NOT command1C's last byte,
    // whose getter002D0013 reads variable25 bit(user+12).
    // Observation only: callers must not invent a native refusal response.
    static std::optional<bool> standbyAcceptsChallenges(const Bytes &b) {
        const auto code=serviceRequestCode(b,true);
        if(code!=2&&code!=3)return {};
        if(b.size()<18||b[5]!=0x0b)return {};
        return (longword(b,14)&1u)==0;
    }
    // Original Saturn outbound 0602D4C8: command1C, length16,
    // three BE32 variable words (49/69/6A), then per-player wait/area
    // and the low bytes returned by selectors 002D000F/002D0013.
    // Do not scan opaque resources for tags or substitute a missing preference.
    static std::optional<uint8_t> standbyWaitPreference(const Bytes &b) {
        if(const auto named=namedRequestBody(b))return standbyWaitPreference(named->first);
        if(!completeMailProbeRequest(b,true))return {};
        const auto card=xband::registrationOffset(b,135);
        const auto pos=xband::registrationOffset(b,161)+longword(b,card+4);
        if(pos<2||pos+18>b.size()||b[pos-2]!=0x0e||b[pos-1]!=3||
           b[pos]!=0x1c||b[pos+1]!=16||b[pos+14]>2)return {};
        return b[pos+14];
    }
    // Read only the framed selected-user byte in a validated outbound1C.
    // Do not search opaque mail/resources or coerce unknown values to 0/1.
    static std::optional<uint8_t> standbyAreaPreference(const Bytes &b) {
        if(const auto named=namedRequestBody(b))return standbyAreaPreference(named->first);
        if(!standbyWaitPreference(b))return {};
        const auto card=xband::registrationOffset(b,135);
        const auto pos=xband::registrationOffset(b,161)+longword(b,card+4);
        return b[pos+15]<=1?std::optional<uint8_t>{b[pos+15]}:std::nullopt;
    }
    Bytes applicationReply() const {
        if(!servicePolicyReply.empty()){
            if(directPeer)throw std::runtime_error("Service policy must not modify peer data");
            return servicePolicyReply;
        }
        auto result=baseApplicationReply();
        // Decide eligibility from the unchanged base response. Prefixes (38,
        // 32, 25, 04) must never disable another independent display update.
        const bool matchReply=!directPeer&&!result.empty()&&
            (result.front()==0x1c||result.front()==0x1d);
        const bool dateReply=matchReply||(!directPeer&&!result.empty()&&result.front()==0x25);
        const auto loginCode=serviceRequestCode(captured,profileMailExperiment);
        const bool displayLogin=matchReply&&(loginCode==2||loginCode==3);
        if(gameIntroTitleReply&&displayLogin){
            const auto prefix=gameIntroTitleReply(diagnostic::receivedGameID(captured));
            result.insert(result.begin(),prefix.begin(),prefix.end());
        }
        if(gameMatchAwardReply&&displayLogin){
            const auto prefix=gameMatchAwardReply(diagnostic::receivedGameID(captured));
            result.insert(result.begin(),prefix.begin(),prefix.end());
        }
        if(displayLogin&&replyReceiverCandidate&&!standbyDialogReply.empty())
            result.insert(result.begin(),standbyDialogReply.begin(),standbyDialogReply.end());
        if(regionTownReply&&displayLogin){
            std::string phone;
            try{phone=xband::registrationPhone(captured);}catch(const std::exception&){phone.clear();}
            if(!phone.empty()){
                const auto prefix=regionTownReply(phone,diagnostic::receivedGameID(captured));
                result.insert(result.begin(),prefix.begin(),prefix.end());
            }
        }
        // Apply only on supported matchmaking login, never mail/debit/results
        // or direct peer traffic. Changing text does NOT enforce area policy.
        if((usageAreaReply||usageAreaPreferenceReply)&&displayLogin){
            std::string phone;
            try{phone=xband::registrationPhone(captured);}catch(const std::exception&){phone.clear();}
            if(!phone.empty()){
                const auto game=diagnostic::receivedGameID(captured);
                const auto prefix=usageAreaPreferenceReply?usageAreaPreferenceReply(phone,game,standbyAreaPreference(captured)):usageAreaReply(phone,game);
                result.insert(result.begin(),prefix.begin(),prefix.end());
            }
        }
        if((levelDisplayFixture||gameRankingReply)&&matchReply){
            const auto id=diagnostic::receivedGameID(captured);
            const auto prefix=gameRankingReply?gameRankingReply(id):diagnostic::levelFixtureWire(id);
            result.insert(result.begin(),prefix.begin(),prefix.end());
        }
        // Do not alter mail, debit, result-only or direct-peer responses.
        if(dateUpdateFixture&&dateReply){
            const auto prefix=dateUpdateFixture->wire();
            result.insert(result.begin(),prefix.begin(),prefix.end());
        }
        if(!directPeer&&!rankingResultReply.empty())
            result.insert(result.begin(),rankingResultReply.begin(),rankingResultReply.end());
        // Preserve matchmaking eligibility/display updates above. Mail must
        // precede the match transition, with exactly one final 02 terminator.
        if(!matchMailPrefix.empty()){
            if(!displayLogin||replyDiagnosticIncomingRecord||diagnosticZeroDebit||!diagnosticMailboxReply.empty())
                throw std::runtime_error("Match mail requires supported matchmaking response");
            result.insert(result.begin(),matchMailPrefix.begin(),matchMailPrefix.end());
        }
        return result;
    }
    Bytes baseApplicationReply() const {
        if(!diagnosticMailboxReply.empty()){
            if(directPeer||serviceRequestCode(captured,profileMailExperiment)!=4||replyDiagnosticIncomingRecord||
               diagnosticZeroDebit||replyState1D||replyReceiverCandidate)
                throw std::runtime_error("Forwarded mail requires isolated mail-only request");
            return diagnosticMailboxReply;
        }
        if(replyDiagnosticIncomingRecord) {
            if(directPeer || serviceRequestCode(captured,profileMailExperiment)!=4 || diagnosticZeroDebit || replyState1D || replyReceiverCandidate)
                throw std::runtime_error("incoming-record fixture requires isolated mail-only service request");
            diagnostic::IncomingRecord record{};
            record.opaqueHeader.fill(0xff);record.opaqueA=0xff;
            record.field1={'D','U','M','M','Y',0};
            record.field2={'L','O','C','A','L',0};
            record.field3={'T','E','S','T',0};
            // Preserve the exact 13-byte original outgoing body object from
            // mail-probe-v2/logs/20260928-030110. No invented executable data.
            // Its interpretation as an incoming body remains an experiment.
            record.field4={0,0,0,6,0,0,0x34,0xa4,0xa3,0xa4,0xc3,0,0xde};
            record.allocationBasis=uint16_t(0x80+record.field4.size()-4);
            record.opaqueD=uint16_t(0x80+record.field4.size());
            const std::array records{record};
            auto result=diagnostic::encodeIncomingRecords(records);
            result.push_back(2); // Existing end-exchange opcode; not mail acceptance.
            return result;
        }
        if(diagnosticZeroDebit) {
            const auto card=xband::registrationOffset(captured,135);
            if(!completeDiagnosticRequest(captured) || captured.size()!=xband::registrationOffset(captured,502) ||
               captured[card+2]!=0 || captured[card+3]!=0 ||
               !std::all_of(captured.begin()+card+8,captured.begin()+card+21,[](uint8_t b){return b==0;}))
                throw std::runtime_error("zero-debit fixture requires observed zero-card block");
            // Original handler49: u32 requested, u32 identity length, bytes.
            // Follow with existing02 terminator; no warning/success injection.
            Bytes result(22,0);result[0]=0x49;putLong(result,1,1);putLong(result,5,13);
            result.push_back(2);return result;
        }
        if(replyReceiverCandidate) {
            Bytes result{0x1c,0,0,0,0,0,0,0,0,0x0e,0,0,0,0,2};
            putLong(result,10,receiverWaitTicks);
            return result;
        }
        if(!replyState1D)return Bytes{2};
        if(replyPeerNumber) {
            if(diagnosticPeerNumber.empty()||diagnosticPeerNumber.size()>24||diagnosticPeerNumber.find_first_not_of("0123456789")!=std::string::npos)
                throw std::runtime_error("invalid diagnostic peer number");
            Bytes result{0x1d,uint8_t(diagnosticPeerNumber.size()+1)};
            result.insert(result.end(),diagnosticPeerNumber.begin(),diagnosticPeerNumber.end());
            result.insert(result.end(),9,0);result.push_back(2);return result;
        }
        return Bytes{0x1d,1,0,0,0,0,0,0,0,0,0,2};
    }
    Bytes servicePolicyReply; // Explicit local rejection; no mail clear or match-role response.
    uint32_t localNext=localInitial+1;
    static uint32_t longword(const Bytes &b,size_t i){return (uint32_t(b[i])<<24)|(uint32_t(b[i+1])<<16)|(uint32_t(b[i+2])<<8)|b[i+3];}
    static void putLong(Bytes &b,size_t i,uint32_t v){D::put(b,i,v>>16);D::put(b,i+2,v);}
    static Bytes pseudo(const Bytes &ip){
        Bytes p(ip.begin()+12,ip.begin()+20);p.push_back(0);p.push_back(6);
        auto n=ip.size()-20;p.push_back(uint8_t(n>>8));p.push_back(uint8_t(n));
        p.insert(p.end(),ip.begin()+20,ip.end());return p;
    }
    static Bytes packet(uint32_t seq,uint32_t ack,uint8_t flags,const Bytes &data={},bool guest=false,unsigned window=4096,bool direct=false,uint16_t guestPort=1025,uint16_t servicePort=1024){
        Bytes b(40,0);b[0]=0x45;D::put(b,2,40+unsigned(data.size()));b[8]=64;b[9]=6;
        b[12]=b[16]=10;b[15]=guest?2:1;b[19]=guest?1:2;
        D::put(b,20,guest?servicePort:2005);D::put(b,22,guest?2005:servicePort);
        if(direct) {
            b[15]=guest?1:2;b[19]=guest?2:1;
            D::put(b,20,guest?guestPort:3000);D::put(b,22,guest?3000:guestPort);
        }
        putLong(b,24,seq);putLong(b,28,ack);b[32]=0x50;b[33]=flags;D::put(b,34,window);
        b.insert(b.end(),data.begin(),data.end());D::put(b,36,D::checksum(pseudo(b)));
        D::put(b,10,D::checksum(Bytes(b.begin(),b.begin()+20)));return b;
    }
    Bytes reply(uint32_t seq,uint32_t ack,uint8_t flags,const Bytes &data={},unsigned window=4096) const {
        bool profileChunk=false;
        if(replyPeerProfile && seq>=localInitial+5) {
            const auto offset=size_t(seq-(localInitial+5));
            profileChunk=offset<=peerProfile.size()&&data.size()<=peerProfile.size()-offset&&
                std::equal(data.begin(),data.end(),peerProfile.begin()+offset);
        }
        const bool marker=replyPeerMarker&&peerMarkerSent&&seq==localInitial+193&&data==Bytes{0x12,0x34,0x56,0x78};
        const bool idleStatus=replyIdleStatus&&directGuestPort==1026&&data==Bytes{0};
        if(directPeer&&!data.empty() && !(replyPeerLength && data==Bytes{0,0,0,0xbc}) && !profileChunk&&!marker&&!idleStatus)throw std::runtime_error("unapproved direct TCP payload");
        return packet(seq,ack,flags,data,false,window,directPeer,directGuestPort,serviceGuestPort);
    }
    Bytes receive(const Bytes &ip){
        if(ip.size()<40||ip.size()>1500||ip[0]!=0x45||D::word(ip,2)!=ip.size()||
           (D::word(ip,6)&0xbfff)!=0||ip[9]!=6||D::checksum(Bytes(ip.begin(),ip.begin()+20))!=0)return {};
        if(longword(ip,12)!=(directPeer?0x0a000001u:0x0a000002u)||longword(ip,16)!=(directPeer?0x0a000002u:0x0a000001u)||D::word(ip,22)!=(directPeer?3000:2005))return {};
        if(directPeer ? D::word(ip,20)!=directGuestPort : (D::word(ip,20)==0||(state!=State::Listen&&D::word(ip,20)!=serviceGuestPort)))return {};
        size_t header=(ip[32]>>4)*4;
        if(header<20||20+header>ip.size()||(ip[32]&15)!=0||D::checksum(pseudo(ip))!=0)return {};
        const auto seq=longword(ip,24),ack=longword(ip,28);const auto flags=ip[33];
        const Bytes data(ip.begin()+20+header,ip.end());
        // Only MSS/NOP/EOL are understood; no timestamp/window-scale/SACK negotiation.
        for(size_t i=40;i<20+header;){
            auto kind=ip[i];if(kind==0)break;if(kind==1){++i;continue;}
            if(i+2>20+header||kind!=2||ip[i+1]!=4||i+4>20+header||flags!=2)return {};
            i+=4;
        }
        if(state==State::Stopped) {
            if(orderlyClose&&!postCloseLogged){postCloseLogged=true;std::cout<<"TCP_POST_CLOSE flags="<<unsigned(flags)<<" bytes="<<data.size()<<"; no new connection accepted\n";}
            return {};
        }
        if(state==State::LastAck) {
            if(flags==0x10&&seq==nextGuest&&ack==localNext&&data.empty()) {
                closeAcknowledged=true;state=State::Stopped;std::cout<<"TCP_CLOSE final ACK received\n";return {};
            }
            if(flags==0x11&&seq+1==nextGuest&&ack==finSequence&&data.empty())
                return reply(finSequence,nextGuest,0x11); // Repeated peer FIN, same sequence.
            if((flags&4)&&seq==nextGuest)state=State::Stopped;
            return {};
        }
        if(flags==2 && data.empty()){
            if(state==State::Listen){serviceGuestPort=uint16_t(D::word(ip,20));guestInitial=seq;nextGuest=seq+1;state=State::SynReceived;}
            if(state==State::SynReceived && seq==guestInitial)return reply(localInitial,nextGuest,0x12);
            return {};
        }
        if(state==State::Listen)return {};
        if((flags&4)!=0){if(seq==nextGuest)state=State::Stopped;return {};}
        if((flags&~0x19)!=0||(flags&0x10)==0||
           (ack!=localNext && !(replyIdleStatus&&statusReplies&&ack==localNext-1) && !(replyPeerProfile&&ack>=localInitial+5&&ack<localNext) && !(((end02Sent && !end02Acknowledged)||(peerLengthSent && !peerLengthAcknowledged)) && ack==localInitial+1)))return {};
        if(end02Sent && ack==localNext)end02Acknowledged=true;
        if(peerLengthSent && ack==localNext)peerLengthAcknowledged=true;
        if(state==State::SynReceived){
            if(seq!=nextGuest)return {};
            state=State::Established;std::cout<<"TCP_OPEN virtual "<<(directPeer?"10.0.0.2:3000; capture-only": "10.0.0.1:2005; service fixture")<<" guest_port="<<(directPeer?directGuestPort:serviceGuestPort)<<'\n';
        }
        if(seq!=nextGuest)return reply(localNext,nextGuest,0x10,{},unsigned(8192-captured.size()));
        if(captured.size()+data.size()>8192){state=State::Stopped;std::cout<<"TCP_STOP capture limit\n";return {};}
        captured.insert(captured.end(),data.begin(),data.end());nextGuest+=uint32_t(data.size());
        if(!data.empty()){
            std::cout<<"TCP_APPLICATION bytes=";
            for(auto b:data)std::cout<<std::hex<<unsigned(b)<<',';
            std::cout<<std::dec<<" total="<<captured.size()<<'\n';
        }
        if(flags&1){
            ++nextGuest;
            if(orderlyClose&&directPeer&&ack==localNext) {
                finSequence=localNext++;state=State::LastAck;
                std::cout<<"TCP_CLOSE guest FIN; sending FIN ACK\n";
                return reply(finSequence,nextGuest,0x11);
            }
            state=State::Stopped;std::cout<<"TCP_STOP guest FIN; ACK only, close handshake not implemented\n";
        }
        // Opt-in idle peer: only keep receive-silence diagnosis separate from readiness.
        // Never send01 (ready/state-change) or echo02 (termination). At most64 replies.
        if(directPeer&&directGuestPort==1026&&replyIdleStatus&&state==State::Established) {
            if(!data.empty()) {
                if(data.size()!=1||data[0]>1){statusHalted=true;statusPending=false;}
                else if(!statusHalted)statusPending=true;
            }
            if(statusPending&&!statusHalted&&statusReplies<64&&ack==localNext&&D::word(ip,34)>0) {
                statusPending=false;++statusReplies;const auto seqOut=localNext++;
                std::cout<<"TCP_IDLE_STATUS reply=00 count="<<statusReplies<<"; synthetic idle peer, not readiness\n";
                return reply(seqOut,nextGuest,0x18,{0});
            }
        }
        if(directPeer && replyPeerLength && !peerLengthSent && state==State::Established && captured==Bytes{0,0,0,0xbc}) {
            peerLengthSent=true;const auto seqOut=localNext;localNext+=4;
            std::cout<<"TCP_PEER_LENGTH_REPLY value=188; synthetic length only, no peer body\n";
            return reply(seqOut,nextGuest,0x18,{0,0,0,0xbc});
        }
        // Exact observed fixture only. No arbitrary payload or downloaded code is accepted.
        if(directPeer && replyPeerProfile && peerLengthAcknowledged && state==State::Established && captured.size()>=192) {
            if(peerProfile.empty()) {
                peerProfile=diagnosticPeerProfile(Bytes(captured.begin()+4,captured.begin()+192));
                std::cout<<"TCP_PEER_PROFILE prepared synthetic 188-byte profile; no marker reply\n";
            }
            if(ack==localNext && peerProfileSent<peerProfile.size()) {
                // Bounded stop-and-wait, at most 32 bytes and within advertised window.
                const size_t count=std::min<size_t>({32,peerProfile.size()-peerProfileSent,D::word(ip,34)});
                if(count) {
                    Bytes chunk(peerProfile.begin()+peerProfileSent,peerProfile.begin()+peerProfileSent+count);
                    const auto seqOut=localNext;localNext+=uint32_t(count);peerProfileSent+=count;
                    std::cout<<"TCP_PEER_PROFILE sent="<<peerProfileSent<<"/188; synthetic only\n";
                    return reply(seqOut,nextGuest,0x18,chunk);
                }
            }
        }
        if(directPeer&&replyPeerMarker&&!peerMarkerSent&&peerProfileSent==188&&ack==localNext&&
           state==State::Established&&captured.size()==196&&
           Bytes(captured.begin()+192,captured.end())==Bytes{0x12,0x34,0x56,0x78}) {
            peerMarkerSent=true;const auto seqOut=localNext;localNext+=4;
            std::cout<<"TCP_PEER_MARKER synthetic confirmation; no subsequent application replies\n";
            return reply(seqOut,nextGuest,0x18,{0x12,0x34,0x56,0x78});
        }
        serviceWindow=D::word(ip,34);
        if(auto response=pollServiceReply();!response.empty())return response;
        if(data.empty() && !(flags&1))return {};
        return reply(localNext,nextGuest,0x10,{},unsigned(8192-captured.size()));
    }
};
