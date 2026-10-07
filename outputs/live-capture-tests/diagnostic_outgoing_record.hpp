#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace diagnostic {
// Original VF REMIX UI capture: player 0, one/two pending mails (2026-09-29).
inline constexpr unsigned maxObservedOutgoingRecords=2;
inline constexpr size_t maxObservedOutgoingTextBytes=32;
inline constexpr size_t maxObservedOutgoingBodyBytes=128; // Original UI produced 42- and 122-byte body fields.
// Observed-format parser only. A controlled original-UI capture established
// fields[0]=recipient codename, [1]=title, [2]=encoded body (2026-09-28).
// They do not establish account identity, an acceptance acknowledgement,
// or a valid incoming 1E record.
struct ObservedOutgoingRecord {
    std::array<uint8_t,10> prefix{};
    std::array<std::vector<uint8_t>,3> fields;
    std::array<uint8_t,4> suffix{}; // Observed zero auxiliary length, not a result code.
};
inline std::vector<ObservedOutgoingRecord> decodeObservedOutgoingList(std::span<const uint8_t> bytes,bool allowObservedPlayer3=false,bool serviceJournal=false){
    size_t position=0;
    auto require=[&](size_t n){if(position>bytes.size()||n>bytes.size()-position)
        throw std::invalid_argument("Truncated observed outgoing list");};
    auto word=[&](){require(2);const auto value=uint16_t((unsigned(bytes[position])<<8)|bytes[position+1]);position+=2;return value;};
    require(1);if(bytes[position++]!=0x1d)throw std::invalid_argument("Not an outgoing 1D list");
    const auto count=word();
    if(serviceJournal){if(bytes.size()>8192||count>(bytes.size()-position)/23)throw std::invalid_argument("Invalid journal list size/count");}
    else if(count>maxObservedOutgoingRecords)throw std::invalid_argument("Only observed zero/one/two-entry lists supported");
    std::vector<ObservedOutgoingRecord> result;
    for(unsigned i=0;i<count;++i){
        ObservedOutgoingRecord record;
        require(record.prefix.size());
        for(auto &b:record.prefix)b=bytes[position++];
        if(serviceJournal?record.prefix[0]>3:(record.prefix[0]!=0&&!(allowObservedPlayer3&&record.prefix[0]==3)))
            throw std::invalid_argument("Unobserved outgoing prefix");
        for(size_t j=1;j<record.prefix.size();++j)
            if(record.prefix[j]!=0xff)throw std::invalid_argument("Unobserved outgoing prefix");
        for(size_t fieldIndex=0;fieldIndex<record.fields.size();++fieldIndex){
            const auto length=word();
            const auto limit=fieldIndex==2?maxObservedOutgoingBodyBytes:maxObservedOutgoingTextBytes;
            if(!length||length>limit)throw std::invalid_argument("Unobserved outgoing field length");
            require(length);auto &field=record.fields[fieldIndex];
            field.assign(bytes.begin()+position,bytes.begin()+position+length);position+=length;
        }
        require(record.suffix.size());for(auto &b:record.suffix)b=bytes[position++];
        for(auto b:record.suffix)if(b!=0)throw std::invalid_argument("Nonempty auxiliary data not supported by capture parser");
        result.push_back(std::move(record));
    }
    if(position!=bytes.size())throw std::invalid_argument("Unexpected outgoing list trailing bytes");
    return result;
}
}
