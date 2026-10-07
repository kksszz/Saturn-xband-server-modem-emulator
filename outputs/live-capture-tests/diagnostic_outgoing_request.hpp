#pragma once
#include "local_tcp_probe.hpp"
#include "diagnostic_outgoing_record.hpp"

namespace diagnostic {
// Locate 1D structurally, after the length-delimited registration and resources.
// Do not scan payload bytes for a tag: names or body bytes may contain 1D/21.
// This is an observed-format decoder, not authentication or delivery.
inline std::vector<ObservedOutgoingRecord> decodeObservedOutgoingRequest(const LocalTCPProbe::Bytes &request,bool allowObservedPlayer3=false,bool serviceJournal=false){
    if(const auto named=LocalTCPProbe::namedRequestBody(request))
        return decodeObservedOutgoingRequest(named->first,allowObservedPlayer3,serviceJournal);
    if(!LocalTCPProbe::completeMailProbeRequest(request,allowObservedPlayer3))
        throw std::invalid_argument("Incomplete or unsupported service request");
    size_t rivalListStart=0,rivalListEnd=0;
    if(LocalTCPProbe::rivalMailListBounds(request,rivalListStart,rivalListEnd))
        return decodeObservedOutgoingList(std::span<const uint8_t>(request).subspan(
            rivalListStart,rivalListEnd-rivalListStart),allowObservedPlayer3,serviceJournal);
    const auto card=xband::registrationOffset(request,135);
    size_t position=xband::registrationOffset(request,184)+LocalTCPProbe::longword(request,card+4);
    auto require=[&](size_t n){if(position>request.size()||n>request.size()-position)
        throw std::invalid_argument("Truncated service resource list");};
    require(3);
    if(request[position]!=0x15)throw std::invalid_argument("Missing resource list");
    const auto count=LocalDiscoveryProbe::word(request,position+1);position+=3;
    for(unsigned i=0;i<count;++i){
        require(6);const auto length=LocalTCPProbe::longword(request,position+2);position+=6;
        require(length);position+=length;
    }
    require(3);
    if(request[position]!=0x16||request[position+1]!=0||request[position+2]!=0)
        throw std::invalid_argument("Unsupported 16 list");
    position+=3;
    // Both captured trailer forms are accepted by the framing parser. Only
    // the established 21 form is decoded here until the 20 form is measured.
    if(request.size()<14||request[request.size()-14]!=0x21||position>request.size()-14)
        throw std::invalid_argument("Unsupported outgoing request trailer");
    return decodeObservedOutgoingList(std::span<const uint8_t>(request).subspan(position,request.size()-14-position),allowObservedPlayer3,serviceJournal);
}
}
