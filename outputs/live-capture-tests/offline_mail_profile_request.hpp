#pragma once
#include "diagnostic_outgoing_record.hpp"
#include "local_tcp_probe.hpp"
#include <xband/mail_profile_names.hpp>

namespace diagnostic {
struct OfflineProfileRequest {
    uint8_t currentPlayer;
    xband::MailProfileNames::Name currentName;
    std::vector<ObservedOutgoingRecord> records;
};
// Read-only inspection. NEVER feed canonicalized bytes to the live server.
inline OfflineProfileRequest inspectProfileRequest(const LocalTCPProbe::Bytes &request){
    using B=LocalTCPProbe::Bytes;
    if(request.size()<200||request.size()>8192)throw std::invalid_argument("Request bound exceeded");
    size_t position=xband::registrationOffset(request,135);
    auto require=[&](size_t n){if(position>request.size()||n>request.size()-position)
        throw std::invalid_argument("Truncated offline profile request");};
    require(8);const auto cardLength=LocalTCPProbe::longword(request,position+4);
    if(cardLength!=0&&cardLength!=13)throw std::invalid_argument("Unsupported card length");
    position=xband::registrationOffset(request,184)+cardLength;
    require(3);
    if(request[position]!=0x15)throw std::invalid_argument("Missing resources");
    const auto count=LocalDiscoveryProbe::word(request,position+1);position+=3;
    if(count>16)throw std::invalid_argument("Resource bound exceeded");
    for(unsigned i=0;i<count;++i){
        require(6);const auto length=LocalTCPProbe::longword(request,position+2);position+=6;
        require(length);position+=length;
    }
    require(6);
    if(B(request.begin()+position,request.begin()+position+3)!=B{0x16,0,0})
        throw std::invalid_argument("Unsupported resource trailer");
    position+=3;const auto start=position;
    if(request[position]!=0x1d)throw std::invalid_argument("Missing outgoing list");
    const auto records=LocalDiscoveryProbe::word(request,position+1);position+=3;
    if(records>4)throw std::invalid_argument("Offline record bound exceeded");
    OfflineProfileRequest result{};
    for(unsigned i=0;i<records;++i){
        const auto begin=position;require(10);position+=10;
        for(unsigned field=0;field<3;++field){
            require(2);const auto length=LocalDiscoveryProbe::word(request,position);position+=2;
            require(length);position+=length;
        }
        require(4);position+=4;
        B single{0x1d,0,1};single.insert(single.end(),request.begin()+begin,request.begin()+position);
        const auto player=single[3];
        if(player>3)throw std::invalid_argument("Offline player outside range");
        single[3]=0;
        auto decoded=decodeObservedOutgoingList(single).front();decoded.prefix[0]=player;
        if(decoded.fields[0].back()!=0||decoded.fields[1].back()!=0)
            throw std::invalid_argument("Unterminated offline mail text");
        result.records.push_back(std::move(decoded));
    }
    if(position+14!=request.size()||request[position]!=0x21)
        throw std::invalid_argument("Unsupported offline request tail");
    B canonical=request;canonical[start+2]=0;
    canonical.erase(canonical.begin()+start+3,canonical.begin()+position);
    if(!LocalTCPProbe::completeDiagnosticRequest(canonical)||LocalTCPProbe::serviceRequestCode(canonical)!=4)
        throw std::invalid_argument("Unsupported registration envelope");
    position=xband::registrationOffset(request,43);require(1);result.currentPlayer=request[position];
    if(result.currentPlayer>3)throw std::invalid_argument("Unobserved current profile number");
    position=xband::registrationOffset(request,81);require(32);
    unsigned length=0;while(length<32&&request[position+length])++length;
    if(!length||length==32)throw std::invalid_argument("Unsupported profile name termination");
    result.currentName.assign(request.begin()+position,request.begin()+position+length);
    return result;
}
}
