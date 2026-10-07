#pragma once
#include "mail_profile_names.hpp"

namespace xband {
// Records offers plus opt-in later observed holdings in a controlled profile.
// No wire tokens, receipt, dequeue or persistence. Pointers are never retained.
class MailProfileOffers {
    std::map<uint64_t,MailProfileNames::Target> offers_;
    std::vector<uint64_t> observedHeld_;
    size_t limit_;
    static void validate(const MailProfileNames::Target &target){
        MailProfileNames validator;
        (void)validator.resolve(target.endpoint,target.epoch,target.player);
    }
public:
    explicit MailProfileOffers(size_t limit=128):limit_(limit){}
    struct SnapshotEntry { uint64_t id; MailProfileNames::Target target; bool observedHeld=false; };
    std::vector<SnapshotEntry> snapshotEntries()const{
        std::vector<SnapshotEntry> result;
        for(const auto &[id,target]:offers_)
            result.push_back({id,target,std::find(observedHeld_.begin(),observedHeld_.end(),id)!=observedHeld_.end()});
        return result;
    }
    void restoreEntries(std::span<const SnapshotEntry> saved){
        if(saved.size()>limit_)throw std::invalid_argument("Offer snapshot capacity");
        MailProfileOffers staged(limit_);
        for(const auto &entry:saved){
            if(!staged.offerBatch(entry.target,std::span<const uint64_t>(&entry.id,1))||staged.size()==0)
                throw std::invalid_argument("Invalid offer snapshot entry");
            if(entry.observedHeld)staged.observedHeld_.push_back(entry.id);
        }
        if(staged.size()!=saved.size())throw std::invalid_argument("Duplicate offer snapshot ID");
        offers_.swap(staged.offers_);observedHeld_.swap(staged.observedHeld_);
    }
    bool offerBatch(const MailProfileNames::Target &target,std::span<const uint64_t> ids){
        validate(target);
        if(ids.size()>limit_)return false;
        auto staged=offers_;
        for(const auto id:ids){
            if(!id)throw std::invalid_argument("Zero mail custody ID");
            const auto found=staged.find(id);
            if(found!=staged.end()){
                if(found->second!=target)return false;
            }else{
                if(staged.size()>=limit_)return false;
                staged.emplace(id,target);
            }
        }
        offers_.swap(staged);return true;
    }
    std::vector<uint64_t> heldFor(const MailProfileNames::Target &target,std::span<const uint64_t> reported)const{
        validate(target);
        if(reported.size()>128)throw std::invalid_argument("Unbounded held mail IDs");
        std::vector<uint64_t> result;
        for(const auto id:reported){
            const auto found=offers_.find(id);
            if(found!=offers_.end()&&found->second==target&&std::find(result.begin(),result.end(),id)==result.end())
                result.push_back(id);
        }
        return result;
    }
    size_t size()const{return offers_.size();}
    // Opt-in policy: retain ONLY IDs subsequently reported by their previously
    // offered controlled profile. Offering alone must never suppress a retry.
    // A later omission may be deletion; it is not a formal delivery/read ACK.
    std::vector<uint64_t> rememberedHeldFor(const MailProfileNames::Target &target,std::span<const uint64_t> reported){
        const auto held=heldFor(target,reported); // Validate before any mutation.
        auto staged=observedHeld_;
        for(const auto id:held)if(std::find(staged.begin(),staged.end(),id)==staged.end())staged.push_back(id);
        if(staged.size()>limit_)throw std::length_error("Held history capacity");
        std::vector<uint64_t> result;
        for(const auto id:staged){
            const auto found=offers_.find(id);
            if(found!=offers_.end()&&found->second==target)result.push_back(id);
        }
        observedHeld_.swap(staged);return result;
    }
    void invalidateAll(){offers_.clear();observedHeld_.clear();}
};
}
