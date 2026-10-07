#pragma once
#include "mail_account_routes.hpp"

namespace xband {
// An inventory observation is NOT a delivery/read receipt. Never dequeue.
// Single-owner snapshot: pointers remain valid only until the store mutates.
struct MailboxSelection {
    size_t matched=0,withheld=0,deferred=0;
    std::vector<const MailCapture*> prepared;
};

// Wire-independent custody-ID policy. The adapter converts its observed token
// scheme to local IDs; this core knows no ROM, socket, JSON or token encoding.
// Filter before limiting so held mail cannot starve later unseen mail.
inline MailboxSelection selectMailbox(const MailCaptureStore &store,
        const MailAccountRoutes &routes,const std::string &endpoint,
        std::span<const uint64_t> heldIds={},size_t responseLimit=4){
    MailboxSelection result;
    const auto candidates=routes.pendingFor(store,endpoint,store.captures().size());
    result.matched=candidates.size();
    for(const auto *capture:candidates){
        if(std::find(heldIds.begin(),heldIds.end(),capture->localId)!=heldIds.end()){
            ++result.withheld;
        }else if(result.prepared.size()<responseLimit){
            result.prepared.push_back(capture);
        }else{
            ++result.deferred;
        }
    }
    return result;
}
}
