#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

// Bounded research encoder for VF XBAND's 0602BFF8 receiver. Sender/title and
// captured binary-body display are proven for test cases; opaque metadata and
// historical address semantics remain unverified. Not a general body encoder.
// Forwarding is opt-in on supported mail/match replies; fixed samples are 04-only.
namespace diagnostic {
using RecordBytes = std::vector<uint8_t>;
inline constexpr size_t saturnInboxCapacity=8;
struct IncomingRecord {
    uint16_t allocationBasis;
    std::array<uint8_t,8> opaqueHeader;
    uint8_t opaqueA,opaqueB;
    uint16_t opaqueC,opaqueD;
    uint32_t opaqueE;
    RecordBytes field1,field2,field3,field4; // town, sender name, title, body object
};
inline RecordBytes encodeIncomingRecords(std::span<const IncomingRecord> records) {
    // Native selected-user inbox holds eight records (060473D0). The
    // journal separately subtracts the uploaded existing inbox count.
    if(records.size()>saturnInboxCapacity)throw std::invalid_argument("Native inbox record-count limit");
    RecordBytes out{0x1e};
    auto word=[&](uint16_t n){out.push_back(uint8_t(n>>8));out.push_back(uint8_t(n));};
    auto longword=[&](uint32_t n){word(uint16_t(n>>16));word(uint16_t(n));};
    auto copy=[&](const RecordBytes &b){out.insert(out.end(),b.begin(),b.end());};
    word(uint16_t(records.size()));
    for(const auto &r:records) {
        const size_t allocation=size_t(r.allocationBasis)+4;
        // The original receiver uses signed word loads after adding four.
        if(allocation<0x80 || allocation>0x7fff || r.field4.size()>allocation-0x80)
            throw std::invalid_argument("Record exceeds original receive allocation");
        // Derived non-overlap limits, not historical protocol maxima.
        if(r.field1.size()>0x22 || r.field2.size()>0x22 || r.field3.size()>0x23)
            throw std::invalid_argument("Field overlaps original fixed destinations");
        // Offline fixture policy: explicit terminators; do not infer text encoding.
        for(const auto *field:{&r.field1,&r.field2,&r.field3})
            if(field->empty() || field->back()!=0)
                throw std::invalid_argument("Diagnostic fields need explicit NUL terminators");
        // Field 4 is a serialized binary object in captured outgoing records,
        // not an established C string. No text terminator is imposed on it.
        word(r.allocationBasis);
        out.insert(out.end(),r.opaqueHeader.begin(),r.opaqueHeader.end());
        out.push_back(r.opaqueA);out.push_back(r.opaqueB);word(r.opaqueC);
        out.push_back(uint8_t(r.field1.size()));copy(r.field1);
        out.push_back(uint8_t(r.field2.size()));copy(r.field2);
        word(r.opaqueD);longword(r.opaqueE);
        out.push_back(uint8_t(r.field3.size()));copy(r.field3);
        word(uint16_t(r.field4.size()));copy(r.field4);
        longword(0); // Original 4D:3 supports absent auxiliary data.
    }
    return out; // No guessed success opcode or terminator appended.
}
}
