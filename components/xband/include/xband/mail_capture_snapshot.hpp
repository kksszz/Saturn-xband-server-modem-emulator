#pragma once
#include "mail_capture_store.hpp"
#include <span>
#include <algorithm>
namespace xband {
// Versioned bounded local custody snapshot. CRC detects corruption, not forgery.
// No emulator, socket, JSON or filesystem dependency. Equal mails stay distinct.
inline uint32_t mailSnapshotCRC(std::span<const uint8_t> bytes){
    uint32_t crc=0xffffffff;
    for(auto byte:bytes){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
    return ~crc;
}
inline std::vector<uint8_t> encodeMailCaptureSnapshot(const MailCaptureStore &store){
    if(store.captures().size()>128)throw std::invalid_argument("Snapshot record limit");
    MailCaptureStore validation;
    std::vector<uint8_t> out{'X','B','M','A','I','L',2,0};
    auto number=[&](uint64_t value,unsigned width){for(unsigned i=width;i>0;--i)out.push_back(uint8_t(value>>((i-1)*8)));};
    auto bytes=[&](std::span<const uint8_t> value){if(value.size()>128)throw std::invalid_argument("Snapshot field limit");out.push_back(uint8_t(value.size()));out.insert(out.end(),value.begin(),value.end());};
    auto text=[&](const std::string &value){bytes({reinterpret_cast<const uint8_t*>(value.data()),value.size()});};
    number(store.captures().size(),2);
    for(const auto &capture:store.captures()){
        const auto id=validation.retain(capture);
        if(!id||capture.localId!=id)throw std::invalid_argument("Nonsequential snapshot identity");
        number(capture.localId,8);text(capture.sourceEndpoint);text(capture.sourcePhone);bytes(capture.sourceCodename);
        out.insert(out.end(),capture.wirePrefix.begin(),capture.wirePrefix.end());
        for(const auto &field:capture.fields)bytes(field);
        out.insert(out.end(),capture.wireSuffix.begin(),capture.wireSuffix.end());
        number(capture.serverAcceptedDate,4);
    }
    number(mailSnapshotCRC(out),4);return out;
}
inline MailCaptureStore decodeMailCaptureSnapshot(std::span<const uint8_t> bytes){
    if(bytes.size()<14||bytes.size()>256*1024)throw std::invalid_argument("Snapshot size limit");
    const size_t end=bytes.size()-4;
    uint32_t expected=0;for(auto b:bytes.last(4))expected=(expected<<8)|b;
    if(mailSnapshotCRC(bytes.first(end))!=expected)throw std::invalid_argument("Snapshot checksum mismatch");
    const std::array<uint8_t,6> header{'X','B','M','A','I','L'};
    if(!std::equal(header.begin(),header.end(),bytes.begin())||(bytes[6]!=1&&bytes[6]!=2)||bytes[7]!=0)
        throw std::invalid_argument("Unknown snapshot version/flags");
    const bool dated=bytes[6]==2;
    size_t pos=8;
    auto require=[&](size_t n){if(pos>end||n>end-pos)throw std::invalid_argument("Truncated snapshot");};
    auto number=[&](unsigned width){require(width);uint64_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|bytes[pos++];return value;};
    auto field=[&](){const auto n=size_t(number(1));if(n>128)throw std::invalid_argument("Snapshot field limit");require(n);std::vector<uint8_t> value(bytes.begin()+pos,bytes.begin()+pos+n);pos+=n;return value;};
    auto text=[&](){const auto value=field();return std::string(value.begin(),value.end());};
    const auto count=number(2);if(count>128)throw std::invalid_argument("Snapshot record limit");
    MailCaptureStore restored;
    for(uint64_t i=0;i<count;++i){
        MailCapture capture;const auto id=number(8);
        capture.sourceEndpoint=text();capture.sourcePhone=text();capture.sourceCodename=field();
        require(capture.wirePrefix.size());std::copy_n(bytes.begin()+pos,capture.wirePrefix.size(),capture.wirePrefix.begin());pos+=capture.wirePrefix.size();
        for(auto &value:capture.fields)value=field();
        require(capture.wireSuffix.size());std::copy_n(bytes.begin()+pos,capture.wireSuffix.size(),capture.wireSuffix.begin());pos+=capture.wireSuffix.size();
        if(dated)capture.serverAcceptedDate=uint32_t(number(4));
        if(id!=i+1||restored.retain(std::move(capture))!=id)throw std::invalid_argument("Invalid snapshot identity/capacity");
    }
    if(pos!=end)throw std::invalid_argument("Trailing snapshot data");
    return restored; // Caller replaces state only after complete validation.
}
}
