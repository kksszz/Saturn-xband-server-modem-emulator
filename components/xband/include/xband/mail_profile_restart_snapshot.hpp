#pragma once
#include "mail_capture_snapshot.hpp"
#include "mail_profile_offers.hpp"

namespace xband {
// Diagnostic continuity for ONE explicit fresh-COW replay, not an account
// identity, delivery ACK, or general-purpose save-state format.
inline std::vector<uint8_t> encodeMailProfileRestartSnapshot(
    const std::string &token,const MailCaptureStore &custody,
    const MailProfileNames &names,const MailProfileOffers &offers){
    if(token.empty()||token.size()>64||token.find('\0')!=std::string::npos)
        throw std::invalid_argument("Invalid restart token");
    const auto custodyBytes=encodeMailCaptureSnapshot(custody);
    const auto profileEntries=names.snapshotEntries();
    const auto offerEntries=offers.snapshotEntries();
    if(profileEntries.size()>128||offerEntries.size()>128)throw std::invalid_argument("Profile restart capacity");
    std::vector<uint8_t> out{'X','B','P','R','O','F',1,0};
    auto number=[&](uint64_t value,unsigned width){for(unsigned i=width;i>0;--i)out.push_back(uint8_t(value>>((i-1)*8)));};
    auto string=[&](const std::string &value){if(value.empty()||value.size()>64||value.find('\0')!=std::string::npos)throw std::invalid_argument("Profile restart key");out.push_back(uint8_t(value.size()));out.insert(out.end(),value.begin(),value.end());};
    string(token);number(custodyBytes.size(),4);number(mailSnapshotCRC(custodyBytes),4);
    number(profileEntries.size(),2);
    for(const auto &entry:profileEntries){
        if(entry.conflicted||entry.name.empty()||entry.name.size()>32)throw std::invalid_argument("Unrestorable profile name");
        string(entry.target.endpoint);string(entry.target.epoch);out.push_back(entry.target.player);
        out.push_back(uint8_t(entry.name.size()));out.insert(out.end(),entry.name.begin(),entry.name.end());
    }
    number(offerEntries.size(),2);
    for(const auto &entry:offerEntries){
        if(!entry.id||entry.id>custody.captures().size()||!names.resolve(entry.target.endpoint,entry.target.epoch,entry.target.player))
            throw std::invalid_argument("Unrestorable profile offer");
        number(entry.id,8);string(entry.target.endpoint);string(entry.target.epoch);
        out.push_back(entry.target.player);out.push_back(entry.observedHeld?1:0);
    }
    number(mailSnapshotCRC(out),4);return out;
}

inline void decodeMailProfileRestartSnapshot(std::span<const uint8_t> bytes,
    const std::string &expectedToken,const MailCaptureStore &custody,
    MailProfileNames &names,MailProfileOffers &offers){
    if(bytes.size()<8+1+4+4+2+2+4||bytes.size()>128*1024)
        throw std::invalid_argument("Profile restart snapshot size");
    const size_t end=bytes.size()-4;
    uint32_t expectedCRC=0;for(auto b:bytes.last(4))expectedCRC=(expectedCRC<<8)|b;
    if(mailSnapshotCRC(bytes.first(end))!=expectedCRC)throw std::invalid_argument("Profile restart checksum");
    const std::array<uint8_t,8> header{'X','B','P','R','O','F',1,0};
    if(!std::equal(header.begin(),header.end(),bytes.begin()))throw std::invalid_argument("Profile restart version");
    size_t pos=8;
    auto require=[&](size_t n){if(pos>end||n>end-pos)throw std::invalid_argument("Profile restart truncation");};
    auto number=[&](unsigned width){require(width);uint64_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|bytes[pos++];return value;};
    auto string=[&](){const auto len=number(1);if(!len||len>64)throw std::invalid_argument("Profile restart key length");require(len);std::string value(bytes.begin()+pos,bytes.begin()+pos+len);pos+=len;if(value.find('\0')!=std::string::npos)throw std::invalid_argument("Profile restart key NUL");return value;};
    if(string()!=expectedToken)throw std::invalid_argument("Profile restart token mismatch");
    const auto custodyBytes=encodeMailCaptureSnapshot(custody);
    if(number(4)!=custodyBytes.size()||number(4)!=mailSnapshotCRC(custodyBytes))
        throw std::invalid_argument("Profile restart custody mismatch");
    const auto nameCount=number(2);if(nameCount>128)throw std::invalid_argument("Profile restart name count");
    std::vector<MailProfileNames::SnapshotEntry> savedNames;
    for(size_t i=0;i<nameCount;++i){
        MailProfileNames::SnapshotEntry entry;
        entry.target.endpoint=string();entry.target.epoch=string();entry.target.player=uint8_t(number(1));
        const auto len=number(1);if(!len||len>32)throw std::invalid_argument("Profile restart name length");
        require(len);entry.name.assign(bytes.begin()+pos,bytes.begin()+pos+len);pos+=len;
        savedNames.push_back(std::move(entry));
    }
    const auto offerCount=number(2);if(offerCount>128)throw std::invalid_argument("Profile restart offer count");
    std::vector<MailProfileOffers::SnapshotEntry> savedOffers;
    for(size_t i=0;i<offerCount;++i){
        MailProfileOffers::SnapshotEntry entry;
        entry.id=number(8);entry.target.endpoint=string();entry.target.epoch=string();
        entry.target.player=uint8_t(number(1));const auto held=number(1);
        if(held>1)throw std::invalid_argument("Profile restart held flag");
        entry.observedHeld=held!=0;savedOffers.push_back(std::move(entry));
    }
    if(pos!=end)throw std::invalid_argument("Profile restart trailing data");
    MailProfileNames stagedNames;stagedNames.restoreEntries(savedNames);
    MailProfileOffers stagedOffers;stagedOffers.restoreEntries(savedOffers);
    for(const auto &entry:savedOffers)
        if(!entry.id||entry.id>custody.captures().size()||!stagedNames.resolve(entry.target.endpoint,entry.target.epoch,entry.target.player))
            throw std::invalid_argument("Profile restart unbound offer");
    names=std::move(stagedNames);offers=std::move(stagedOffers);
}
}
