#include <xband/mailbox_selection.hpp>
#include <iostream>

int main(){try{
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("mailbox selection failed");};
    xband::MailCaptureStore store;
    xband::MailAccountRoutes routes;
    const std::vector<uint8_t> sender{'8','8'},recipient{'9','9'};
    routes.observe("sender",sender);routes.observe("recipient",recipient);
    xband::MailCapture capture;
    capture.sourceEndpoint="sender";capture.sourcePhone="3336666666";
    capture.sourceCodename=sender;capture.fields={std::vector<uint8_t>{'9','9',0},
        std::vector<uint8_t>{'9','8',0},std::vector<uint8_t>{0,7}};
    for(uint64_t i=1;i<=6;++i)check(store.retain(capture)==i);
    const auto bytes=store.retainedBytes();
    auto selection=xband::selectMailbox(store,routes,"recipient");
    check(selection.matched==6&&selection.withheld==0&&selection.deferred==2&&selection.prepared.size()==4);
    const std::vector<uint64_t> held{1,2,3,4,4,999};
    selection=xband::selectMailbox(store,routes,"recipient",held,1);
    check(selection.matched==6&&selection.withheld==4&&selection.deferred==1&&selection.prepared.size()==1);
    check(selection.prepared[0]->localId==5);
    selection=xband::selectMailbox(store,routes,"recipient",held,0);
    check(selection.prepared.empty()&&selection.withheld==4&&selection.deferred==2);
    check(xband::selectMailbox(store,routes,"sender").matched==0);
    check(xband::selectMailbox(store,routes,"unknown").matched==0);
    routes.observe("duplicate",recipient);
    check(xband::selectMailbox(store,routes,"recipient").matched==0);
    routes.observe("duplicate",sender);
    check(xband::selectMailbox(store,routes,"recipient",held).prepared.size()==2);
    const std::vector<uint64_t> all{1,2,3,4,5,6};
    selection=xband::selectMailbox(store,routes,"recipient",all);
    check(selection.withheld==6&&selection.prepared.empty()&&selection.deferred==0);
    // No acknowledgement/deletion: missing inventory offers all mail again.
    check(xband::selectMailbox(store,routes,"recipient").prepared.size()==4);
    check(store.captures().size()==6&&store.retainedBytes()==bytes);
    std::cout<<"PASS mailbox selection: route isolation/ambiguity, held-before-limit, bounded response, no dequeue\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
