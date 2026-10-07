#pragma once
#include "local_tcp_probe.hpp"
#include <iomanip>
#include <sstream>

namespace diagnostic {
// Shared Saturn XOS: 00270001 -> 060473D0 clamps the selected user's
// inbox count to eight. 0602C964..C99E uploads that count and its words.
// This is a per-user inbox bound, never a server storage or outbox limit.
inline constexpr size_t nativeInboxCapacity=saturnInboxCapacity;
using MailAccountKey=std::pair<std::string,unsigned>;
inline MailAccountKey mailAccountKey(const LocalTCPProbe::Bytes& request){
    const auto at=xband::registrationOffset(request,35);
    const auto slot=xband::registrationOffset(request,43);
    if(slot>=request.size()||request[slot]>3||at+8>request.size())
        throw std::invalid_argument("Invalid mail account slot");
    // 0B uploads the profile's first twelve bytes; the eight-byte box
    // identity precedes its user slot. Unassigned boxes report all FF.
    const auto begin=request.begin()+at,end=begin+8;
    const bool unassigned=std::all_of(begin,end,[](uint8_t b){return b==0xff;})||
                          std::all_of(begin,end,[](uint8_t b){return b==0;});
    std::string terminal;
    if(unassigned)terminal="phone:"+xband::phoneDigits(xband::registrationPhone(request));
    else{
        std::ostringstream value;value<<"box:"<<std::hex<<std::setfill('0');
        for(auto it=begin;it!=end;++it)value<<std::setw(2)<<unsigned(*it);
        terminal=value.str();
    }
    return {terminal,request[slot]}; // Declared identity, NOT authentication.
}
inline size_t reportedInboxCount(const LocalTCPProbe::Bytes& request){
    const auto code=LocalTCPProbe::serviceRequestCode(request);
    if(code!=2&&code!=3&&code!=4)throw std::invalid_argument("Incomplete mail inventory request");
    (void)mailAccountKey(request);
    const auto at=xband::registrationOffset(request,115);
    if(at+2>request.size())throw std::invalid_argument("Missing inbox count");
    const auto count=LocalTCPProbe::D::word(request,at);
    if(count>16||at+2+size_t(count)*2>request.size())throw std::invalid_argument("Invalid inbox inventory");
    return count; // Count records, even if opaque words have equal values.
}
inline size_t availableInboxSlots(const LocalTCPProbe::Bytes& request){
    const auto count=reportedInboxCount(request);
    return count<nativeInboxCapacity?nativeInboxCapacity-count:0;
}
}
