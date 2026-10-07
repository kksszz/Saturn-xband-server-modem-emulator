#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace xband {
// Single-owner, bounded, in-memory server custody. No emulator/socket/JSON
// dependency. Observations are NOT delivery receipts. The observed wire format
// has no established unique message ID: retain equal submissions separately.
struct MailCapture {
    uint64_t localId=0;
    // Server acceptance day, packed for VF's mail date field; zero = unknown.
    // Not the user's composition time or a historical XBAND timestamp.
    uint32_t serverAcceptedDate=0;
    std::string sourceEndpoint,sourcePhone;
    std::vector<uint8_t> sourceCodename;
    std::array<uint8_t,10> wirePrefix{};
    std::array<std::vector<uint8_t>,3> fields; // recipient, title, encoded body
    std::array<uint8_t,4> wireSuffix{};
};
class MailCaptureStore {
    std::vector<MailCapture> captures_;
    size_t bytes_=0;
    size_t recordLimit_,byteLimit_;
public:
    explicit MailCaptureStore(size_t recordLimit=128,size_t byteLimit=256*1024):
        recordLimit_(recordLimit),byteLimit_(byteLimit){}
    // Zero means capacity refused. Invalid data throws; neither case mutates.
    uint64_t retain(MailCapture capture){
        if(capture.sourceEndpoint.empty()||capture.sourceEndpoint.size()>64||
           capture.sourcePhone.empty()||capture.sourcePhone.size()>32||
           capture.sourceCodename.empty()||capture.sourceCodename.size()>32)
            throw std::invalid_argument("Invalid mail source metadata");
        size_t size=capture.sourceEndpoint.size()+capture.sourcePhone.size()+capture.sourceCodename.size()+14;
        for(size_t index=0;index<capture.fields.size();++index){
            const auto &field=capture.fields[index];
            const auto limit=index==2?128u:32u; // Original VF UI sent a 122-byte encoded body.
            if(field.empty()||field.size()>limit)throw std::invalid_argument("Unsupported captured mail field");
            size+=field.size();
        }
        if(capture.fields[0].back()!=0||capture.fields[1].back()!=0)
            throw std::invalid_argument("Unterminated captured mail text");
        if(captures_.size()>=recordLimit_||bytes_>byteLimit_||size>byteLimit_-bytes_)return 0;
        capture.localId=uint64_t(captures_.size())+1;
        captures_.push_back(std::move(capture));bytes_+=size;
        return captures_.back().localId;
    }
    const std::vector<MailCapture> &captures()const noexcept{return captures_;}
    // Whole-request memory admission. Empty/unsupported/capacity refusal must
    // never become a partial acceptance receipt. Persistent commit belongs to
    // the owner, which stages this store and publishes only after saving it.
    // Empty result with nonempty input means capacity refused, without mutation.
    std::vector<uint64_t> retainBatch(const std::vector<MailCapture> &captures){
        if(captures.empty())return {};
        auto candidate=*this;
        std::vector<uint64_t> ids;
        for(const auto &capture:captures){
            const auto id=candidate.retain(capture);
            if(!id)return {};
            ids.push_back(id);
        }
        *this=std::move(candidate);
        return ids;
    }
    size_t retainedBytes()const noexcept{return bytes_;}
};
}
