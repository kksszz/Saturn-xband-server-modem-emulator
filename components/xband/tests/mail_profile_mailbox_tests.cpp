#include <xband/mail_profile_mailbox.hpp>
#include <xband/mail_profile_offers.hpp>
#include <iostream>

int main(){try{
    using B=std::vector<uint8_t>;
    auto check=[](bool ok){if(!ok)throw std::runtime_error("Profile mailbox assertion failed");};
    const xband::MailProfileNames::Target target{"m1","epoch",3};
    xband::MailProfileOffers offers(2);
    check(offers.offerBatch(target,std::vector<uint64_t>{1,2,1})==false&&offers.size()==0);
    check(offers.offerBatch(target,std::vector<uint64_t>{1,2})&&offers.size()==2);
    check(offers.offerBatch(target,std::vector<uint64_t>{1,2}));
    check(!offers.offerBatch({"m2","epoch",3},std::vector<uint64_t>{1})&&offers.size()==2);
    check(!offers.offerBatch(target,std::vector<uint64_t>{1,3})&&offers.size()==2);
    check(offers.heldFor(target,std::vector<uint64_t>{1,1,2,3,999})==std::vector<uint64_t>({1,2}));
    check(offers.heldFor({"m1","epoch",0},std::vector<uint64_t>{1,2}).empty());
    check(offers.heldFor({"m1","other",3},std::vector<uint64_t>{1,2}).empty());
    check(offers.rememberedHeldFor(target,std::vector<uint64_t>{}).empty()); // Offered != retained by guest.
    check(offers.rememberedHeldFor(target,std::vector<uint64_t>{1,1,999})==std::vector<uint64_t>{1});
    check(offers.rememberedHeldFor(target,std::vector<uint64_t>{})==std::vector<uint64_t>{1}); // Omission after deletion.
    check(offers.heldFor(target,std::vector<uint64_t>{}).empty()); // Legacy policy unchanged.
    check(offers.rememberedHeldFor({"m2","epoch",3},std::vector<uint64_t>{1,2}).empty());
    check(offers.rememberedHeldFor({"m1","epoch",0},std::vector<uint64_t>{1,2}).empty());
    check(offers.rememberedHeldFor({"m1","other",3},std::vector<uint64_t>{1,2}).empty());
    bool historyRejected=false;
    try{offers.rememberedHeldFor(target,std::vector<uint64_t>(129,2));}catch(const std::invalid_argument&){historyRejected=true;}
    check(historyRejected&&offers.rememberedHeldFor(target,std::vector<uint64_t>{})==std::vector<uint64_t>{1});
    check(offers.rememberedHeldFor(target,std::vector<uint64_t>{2})==std::vector<uint64_t>({1,2}));
    offers.invalidateAll();check(offers.size()==0&&offers.heldFor(target,std::vector<uint64_t>{1,2}).empty());
    check(offers.rememberedHeldFor(target,std::vector<uint64_t>{1,2}).empty());
    bool rejected=false;
    try{offers.offerBatch(target,std::vector<uint64_t>{1,0});}catch(const std::invalid_argument&){rejected=true;}
    check(rejected&&offers.size()==0);
    rejected=false;try{offers.heldFor(target,std::vector<uint64_t>(129,1));}catch(const std::invalid_argument&){rejected=true;}
    check(rejected);
    rejected=false;try{offers.offerBatch({"m1","epoch",4},std::vector<uint64_t>{1});}catch(const std::invalid_argument&){rejected=true;}
    check(rejected&&offers.size()==0);
    xband::MailProfileNames names;
    names.observe("m1","epoch",0,B{'8','8'});names.observe("m1","epoch",3,B{'9','9'});
    names.observe("m2","epoch",0,B{'R'});
    xband::MailCaptureStore store;
    xband::MailCapture capture{};
    capture.sourceEndpoint="m1";capture.sourcePhone="123";capture.sourceCodename={'S'};
    capture.fields={B{'9','9',0},B{'T',0},B{0,7}};check(store.retain(capture)==1);
    capture.fields[0]={'8','8',0};check(store.retain(capture)==2);
    capture.fields[0]={'R',0};check(store.retain(capture)==3);
    capture.fields[0]={'U',0};check(store.retain(capture)==4);
    capture.fields[0]={'9','9',0};check(store.retain(capture)==5);
    auto three=xband::selectProfileMailbox(store,names,{"m1","epoch",3},1);
    check(three.matched==2&&three.prepared.size()==1&&three.prepared[0]->localId==1&&three.deferred==1);
    auto zero=xband::selectProfileMailbox(store,names,{"m1","epoch",0});
    check(zero.matched==1&&zero.prepared[0]->localId==2);
    auto remote=xband::selectProfileMailbox(store,names,{"m2","epoch",0});
    check(remote.matched==1&&remote.prepared[0]->localId==3);
    check(xband::selectProfileMailbox(store,names,{"m1","other-epoch",3}).prepared.empty());
    check(xband::selectProfileMailbox(store,names,{"unknown","epoch",3}).prepared.empty());
    auto none=xband::selectProfileMailbox(store,names,{"m1","epoch",3},0);
    check(none.prepared.empty()&&none.matched==2&&none.deferred==2);
    const std::vector<uint64_t> held{1,1,2,3,4,999};
    auto filtered=xband::selectProfileMailbox(store,names,{"m1","epoch",3},1,held);
    check(filtered.matched==2&&filtered.withheld==1&&filtered.deferred==0&&filtered.prepared.size()==1&&filtered.prepared[0]->localId==5);
    const std::vector<uint64_t> allHeld{1,5};
    auto all=xband::selectProfileMailbox(store,names,{"m1","epoch",3},0,allHeld);
    check(all.matched==2&&all.withheld==2&&all.deferred==0&&all.prepared.empty());
    auto other=xband::selectProfileMailbox(store,names,{"m1","epoch",0},4,allHeld);
    check(other.matched==1&&other.withheld==0&&other.prepared[0]->localId==2);
    auto heldUnknown=xband::selectProfileMailbox(store,names,{"unknown","epoch",3},4,allHeld);
    check(heldUnknown.prepared.empty()&&heldUnknown.withheld==0);
    names.observe("m1","epoch",1,B{'9','9'});
    check(xband::selectProfileMailbox(store,names,{"m1","epoch",3}).prepared.empty());
    names.observe("m1","epoch",1,B{'X'}); // Conflicted epoch must fail closed globally.
    check(xband::selectProfileMailbox(store,names,{"m2","epoch",0}).prepared.empty());
    check(store.captures().size()==5); // No delivery/delete/deduplication.
    std::cout<<"PASS portable profile mailboxes: current player/endpoint/epoch, held-ID filter before limit, duplicate/foreign/unknown IDs, unknown/ambiguous/conflicted refusal, no dequeue\n";
    return 0;
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
