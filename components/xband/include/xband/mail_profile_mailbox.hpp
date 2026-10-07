#pragma once
#include "mail_profile_names.hpp"
#include "mailbox_selection.hpp"

namespace xband {
// Mail for the current LOCAL profile only. All names must be observed in this
// owner-controlled epoch; this is not authentication. No acknowledgement,
// deduplication, held-ID inference or dequeue. Invalid/ambiguous addresses hold.
// Optional held IDs must already be validated and scoped to this exact current
// profile by the adapter. Raw opaque guest words are NOT custody IDs.
inline MailboxSelection selectProfileMailbox(const MailCaptureStore &store,
        const MailProfileNames &names,const MailProfileNames::Target &current,size_t limit=4,
        std::span<const uint64_t> heldIds={}){
    MailboxSelection result;
    if(!names.resolve(current.endpoint,current.epoch,current.player))return result;
    for(const auto &capture:store.captures()){
        const auto &to=capture.fields[0];
        if(to.size()<2||to.back()!=0||std::find(to.begin(),to.end()-1,0)!=to.end()-1)continue;
        const auto target=names.uniqueTarget(current.epoch,std::span<const uint8_t>(to).first(to.size()-1));
        if(!target||*target!=current)continue;
        ++result.matched;
        if(std::find(heldIds.begin(),heldIds.end(),capture.localId)!=heldIds.end()){
            ++result.withheld;continue;
        }
        if(result.prepared.size()<limit)result.prepared.push_back(&capture);
        else ++result.deferred;
    }
    return result;
}
}
