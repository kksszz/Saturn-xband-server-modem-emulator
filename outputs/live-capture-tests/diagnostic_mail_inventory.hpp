#pragma once
#include "diagnostic_mail_forward.hpp"
#include "local_tcp_probe.hpp"
#include <algorithm>
namespace diagnostic {
// Synthetic session-local tokens, NOT historical XBAND IDs or read receipts.
// Fresh COW guest state is required. Server restarts must restore the custody
// snapshot, not reset the ID sequence. Never use this mode with saved user mail.
inline uint16_t inventoryToken(const xband::MailCapture &capture){
    if(!capture.localId||capture.localId>128)throw std::invalid_argument("Unsupported diagnostic token ID");
    return uint16_t(0x1000+capture.localId);
}
inline std::vector<uint16_t> reportedInventory(const LocalTCPProbe::Bytes &request,bool profileExperiment=false){
    if(LocalTCPProbe::serviceRequestCode(request,profileExperiment)!=4)throw std::invalid_argument("Inventory requires complete mail-only registration");
    const auto list=xband::registrationOffset(request,115);
    const auto count=LocalDiscoveryProbe::word(request,list);
    if(count>16||list+2+size_t(count)*2>request.size())throw std::invalid_argument("Invalid inventory list");
    std::vector<uint16_t> words;
    for(unsigned i=0;i<count;++i)words.push_back(LocalDiscoveryProbe::word(request,list+2+i*2));
    return words;
}
inline IncomingRecord incomingWithInventoryToken(const xband::MailCapture &capture){
    auto record=incomingFromCapture(capture);record.opaqueD=inventoryToken(capture);return record;
}
inline bool inventoryContains(std::span<const uint16_t> words,const xband::MailCapture &capture){
    return std::find(words.begin(),words.end(),inventoryToken(capture))!=words.end();
}
inline std::vector<uint64_t> custodyIdsFromInventory(std::span<const uint16_t> words){
    std::vector<uint64_t> ids;
    for(const auto word:words)if(word>0x1000&&word<=0x1080)ids.push_back(word-0x1000);
    return ids; // Unknown tokens cannot suppress any of this store's IDs.
}
}
