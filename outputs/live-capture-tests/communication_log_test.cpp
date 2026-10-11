#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "communication_log_window.hpp"
#include <iostream>
#include <cassert>
#include <set>
struct Endpoint: xband::ServiceEndpoint {
    bool accept=false;std::deque<uint8_t> bytes{0,255,65};
    bool transmit(uint8_t,unsigned)override{return accept;}
    void tick(unsigned)override{}
    bool peek(uint8_t& b)const override{if(bytes.empty())return false;b=bytes.front();return true;}
    void consume()override{bytes.pop_front();}
    size_t pending()const override{return bytes.size();}
    void reset()override{}
};
void check(bool value){if(!value)throw std::runtime_error("communication log regression");}
int main(){
    const auto base=std::filesystem::temp_directory_path()/("xband-communication-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    try{
        auto log=std::make_shared<diagnostic::CommunicationLog>(base);
        auto raw=std::make_unique<Endpoint>();auto* ptr=raw.get();
        {
            diagnostic::LoggedEndpoint endpoint(std::move(raw),log,"Service",0);
            check(!endpoint.transmit(42,1));ptr->accept=true;check(endpoint.transmit(43,2));
            uint8_t b;check(endpoint.peek(b)&&b==0);check(endpoint.peek(b)&&b==0);
            endpoint.consume();endpoint.consume();endpoint.consume();
            endpoint.reset();check(endpoint.transmit(44,3));
        }
        for(unsigned i=0;i<410;++i)log->byte("Peer",1,"TX",uint8_t(i),100+i,17);
        log->stop();const auto snap=log->snapshot();check(snap.error.empty());check(snap.pages.size()>=3);
        size_t serviceTx=0,serviceRx=0,peer=0;std::set<uint64_t> sessions;std::string rxText;
        for(const auto& file:snap.pages)for(const auto& row:diagnostic::CommunicationLog::readPage(file)){
            if(row["channel"]=="Peer"){++peer;check(row["side"]==1);}
            else if(row["direction"]=="TX"){++serviceTx;check(row["hex"]!="2A ");sessions.insert(row["session"].get<uint64_t>());}
            else {serviceRx+=row["hex"].get<std::string>().size()/3;rxText+=row["text"].get<std::string>();}
        }
        check(serviceTx==2&&sessions.size()==2&&serviceRx==3&&rxText=="\\x00\\xFFA"&&peer==410);
        diagnostic::CommunicationLogWindow viewer;viewer.open(nullptr,log,false);check(viewer.handle()!=nullptr);
        const auto window=viewer.handle();SendMessageW(window,WM_COMMAND,11,0);
        check(SendMessageW(GetDlgItem(window,5),BM_GETCHECK,0,0)==BST_UNCHECKED);
        SendMessageW(GetDlgItem(window,1),CB_SETCURSEL,2,0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(1,CBN_SELCHANGE),0);
        SetWindowTextW(GetDlgItem(window,4),L"Peer");
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(4,EN_CHANGE),0);
        check(GetWindowTextLengthW(GetDlgItem(window,7))>0);
        SendMessageW(window,WM_COMMAND,13,0);SendMessageW(window,WM_CLOSE,0,0);check(!viewer.handle());
        auto second=std::make_shared<diagnostic::CommunicationLog>(base);second->byte("Control",0,"RX",9,0,1);second->stop();check(second->snapshot().directory!=snap.directory);
        check(std::filesystem::exists(snap.pages.front()));
        // A failed capture cannot prevent a wrapped endpoint accepting data.
        auto failure=std::make_shared<diagnostic::CommunicationLog>(base);
        std::filesystem::create_directory(failure->snapshot().directory/"0000000001.jsonl");
        failure->byte("Service",0,"TX",99,0,1);
        for(unsigned tries=0;tries<200&&failure->snapshot().error.empty();++tries)Sleep(10);
        check(!failure->snapshot().error.empty());
        auto normal=std::make_unique<Endpoint>();normal->accept=true;
        diagnostic::LoggedEndpoint stillWorks(std::move(normal),failure,"Service",0);
        check(stillWorks.transmit(88,2));failure->stop();
        const auto bad=base/"blocked";{std::ofstream file(bad);file<<"not a directory";}
        bool rejected=false;try{diagnostic::CommunicationLog impossible(bad);}catch(...){rejected=true;}check(rejected);
        std::filesystem::remove_all(base);
        std::cout<<"communication log: accepted bytes, RX consume, binary escapes, sessions, paging, isolated runs and hidden UI passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<" preserved fixture "<<base.string()<<'\n';return 1;}
}
