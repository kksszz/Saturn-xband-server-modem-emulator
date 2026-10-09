#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace media_card {
struct CardReport {
    int32_t value=0;
    std::optional<std::array<uint8_t,13>> raw;
};
// Login/report context only: opcode1E also has a different debit-reply layout.
// This decodes data, not authenticity or authority to charge an account.
inline CardReport registrationCardReport(std::span<const uint8_t> bytes){
    if(bytes.size()<8||bytes[0]!=0x1e||bytes[1]!=0)
        throw std::invalid_argument("Expected original Saturn card report");
    uint32_t length=0;for(unsigned i=4;i<8;++i)length=(length<<8)|bytes[i];
    if((length!=0&&length!=13)||bytes.size()!=8+length)
        throw std::invalid_argument("Incomplete or unsupported card report length");
    const uint32_t word=(uint32_t(bytes[2])<<8)|bytes[3];
    CardReport report;report.value=word&0x8000u?int32_t(word)-65536:int32_t(word);
    if(length){report.raw.emplace();for(unsigned i=0;i<13;++i)(*report.raw)[i]=bytes[8+i];}
    return report;
}
// Experimental codec, not a charge scheduler or historical pricing policy.
// No retries: a matching prefix can debit again. Stale full-record rejection
// is a data comparison, not transaction deduplication or delivery proof.
inline std::vector<uint8_t> debitRequest(uint32_t amount,std::span<const uint8_t> cardPrefix){
    // Local safety envelope: tested signed card values and 13-byte card record.
    // These bounds are not claimed as historical server configuration limits.
    if(amount>32767||cardPrefix.empty()||cardPrefix.size()>13)
        throw std::invalid_argument("Outside verified virtual-card request envelope");
    std::vector<uint8_t> bytes{0x49};
    auto append=[&](uint32_t value){for(unsigned shift:{24u,16u,8u,0u})bytes.push_back(uint8_t(value>>shift));};
    append(amount);append(uint32_t(cardPrefix.size()));
    bytes.insert(bytes.end(),cardPrefix.begin(),cardPrefix.end());return bytes;
}
inline int64_t debitReply(std::span<const uint8_t> bytes){
    if(bytes.size()!=5||bytes[0]!=0x1e)throw std::invalid_argument("Expected1E and four-byte debit result");
    uint32_t value=0;for(unsigned i=1;i<5;++i)value=(value<<8)|bytes[i];
    // Explicit sign conversion without implementation-defined unsigned cast.
    return value&0x80000000u?int64_t(value)-0x100000000LL:int64_t(value);
}
}
