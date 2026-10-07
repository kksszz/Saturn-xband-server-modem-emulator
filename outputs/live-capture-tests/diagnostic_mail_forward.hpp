#pragma once
#include <algorithm>
#include "diagnostic_incoming_record.hpp"
#include "../../components/xband/include/xband/mail_capture_store.hpp"
namespace diagnostic {
// Experimental 1D -> 1E conversion using the proven incoming fixture's opaque
// metadata except the verified date at receive-record +58. Address is a
// separately supplied EUC-JP region, never a fallback copy of the name.
// Acceptance day is not historical send-time semantics.
inline IncomingRecord incomingFromCapture(const xband::MailCapture &capture,std::span<const uint8_t> address={}){
    for(auto b:capture.sourceCodename)if(!b)throw std::invalid_argument("Embedded sender terminator");
    if(capture.sourceCodename.empty()||capture.sourceCodename.size()>32||
       capture.fields[1].empty()||capture.fields[1].back()!=0||
       capture.fields[2].empty()||capture.fields[2].size()>128)
        throw std::invalid_argument("Unsupported forwarding capture");
    if(address.size()>33||std::find(address.begin(),address.end(),0)!=address.end())
        throw std::invalid_argument("Invalid forwarding address");
    IncomingRecord record{};record.opaqueHeader.fill(0xff);record.opaqueA=0xff;
    record.opaqueE=capture.serverAcceptedDate;
    record.field1.assign(address.begin(),address.end());record.field1.push_back(0);
    record.field2=capture.sourceCodename;record.field2.push_back(0);
    record.field3=capture.fields[1];record.field4=capture.fields[2];
    record.allocationBasis=uint16_t(0x80+record.field4.size()-4);
    record.opaqueD=uint16_t(0x80+record.field4.size());
    return record;
}
}
