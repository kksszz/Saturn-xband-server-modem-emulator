// SPDX-License-Identifier: GPL-3.0-only
#include "local_sci_cable.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
using saturn::link::LocalSciCable;
using State = saturn::link::LinkCableService::State;
static void check(bool value, const char* reason) { if(!value)throw std::runtime_error(reason); }
static int keepRunning(){return 0;}
static int cancelNow(){return 1;}
static void bind(LocalSciCable& a,LocalSciCable& b,unsigned ac=0,unsigned bc=0){
    auto first=std::async(std::launch::async,[&]{return a.Advance(100,1,ac,keepRunning);});
    b.Advance(900,1,bc,keepRunning);
    check(first.wait_for(std::chrono::seconds(2))==std::future_status::ready,"handshake deadline");
    first.get();
}
static void barrier(LocalSciCable& a,LocalSciCable& b,std::uint64_t relative){
    auto first=std::async(std::launch::async,[&]{return a.Advance(100+relative,1,0,keepRunning);});
    b.Advance(900+relative,1,0,keepRunning);
    check(first.wait_for(std::chrono::seconds(2))==std::future_status::ready,"progress deadline");first.get();
}
static void exitedPeer(unsigned channel){
    LocalSciCable cable(channel,128);
    wchar_t executable[32768];
    check(GetModuleFileNameW(nullptr,executable,32768)!=0,"locate process test executable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --exit-peer "+std::to_wstring(channel);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    check(CreateProcessW(nullptr,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                         nullptr,nullptr,&startup,&process)!=0,"start process test peer");
    try{
        const auto connectDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!cable.Connected()&&std::chrono::steady_clock::now()<connectDeadline){
            cable.Advance(100,1,0,keepRunning);Sleep(1);
        }
        check(cable.Connected(),"cross-process shared handshake");
        check(TerminateProcess(process.hProcess,0)!=0,"terminate only spawned test peer");
        check(WaitForSingleObject(process.hProcess,2000)==WAIT_OBJECT_0,"peer exit deadline");
        const auto start=std::chrono::steady_clock::now();
        cable.Advance(228,1,0,keepRunning);
        check(cable.GetState()==State::Error,"abrupt process exit invalidates session");
        check(std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"exit must not wait for timeout");
    }catch(...){
        TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,2000);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);throw;
    }
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
int main(int argc,char** argv)try{
    if(argc==3&&std::string(argv[1])=="--exit-peer"){
        LocalSciCable peer(unsigned(std::stoul(argv[2])),128);
        peer.Advance(900,1,0,keepRunning);
        Sleep(10000);return 0;
    }
    const unsigned channel=44000+GetCurrentProcessId()%10000;
    for(unsigned q:{64u,128u,256u,512u,1024u,2048u,4096u}){
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            check(a.Connected()&&b.Connected(),"bound dual endpoints");
            bool refused=false;try{LocalSciCable third(channel,q);}catch(const std::exception&){refused=true;}
            check(refused&&a.Connected()&&b.Connected(),"third endpoint refusal");
            refused=false;try{LocalSciCable mismatch(channel,q==4096?128:4096);}catch(const std::exception&){refused=true;}
            check(refused&&a.Connected(),"quantum mismatch without damaging peers");
            a.Send(true,42,104);a.Send(false,77,104);a.Send(true,43,110);
            b.Send(true,99,904);b.Send(false,0,904);
            barrier(a,b,q);
            check(b.Receive(true,900+q+3)==-1,"not before delivery deadline");
            check(b.Receive(false,900+q+4)==77,"slave lane independent");
            check(b.Receive(true,900+q+4)==42,"master lane");
            check(b.Receive(true,900+q+9)==-1,"ordered future byte");
            check(b.Receive(true,900+q+10)==43,"second master byte");
            check(a.Receive(true,100+q+4)==99&&a.Receive(false,100+q+4)==0,"duplex lanes");
            for(unsigned i=0;i<1000;++i){
                a.Send(true,std::uint8_t(i),100+q+11);a.Send(false,std::uint8_t(i+1),100+q+11);
                b.Send(true,std::uint8_t(i+2),900+q+11);b.Send(false,std::uint8_t(i+3),900+q+11);
            }
            barrier(a,b,2*q);
            for(unsigned i=0;i<1000;++i){
                check(b.Receive(true,900+2*q+11)==int(std::uint8_t(i)),"master order A");
                check(b.Receive(false,900+2*q+11)==int(std::uint8_t(i+1)),"slave order A");
                check(a.Receive(true,100+2*q+11)==int(std::uint8_t(i+2)),"master order B");
                check(a.Receive(false,100+2*q+11)==int(std::uint8_t(i+3)),"slave order B");
            }
            b.Disconnect();a.Advance(100+3*q,1,0,keepRunning);
            check(a.GetState()==State::Error&&a.Receive(true,100+4*q)==-1,"peer reset invalidates bytes");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            a.Advance(100+2*q,1,0,keepRunning);
            check(a.GetState()==State::Error,"overshot budget");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            a.Advance(101,2,0,keepRunning);check(a.GetState()==State::Error,"revision invalidates");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            a.Advance(99,1,0,keepRunning);check(a.GetState()==State::Error,"clock reversed");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            a.Advance(101,1,1,keepRunning);check(a.GetState()==State::Error,"clock mode changed");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b,0,1);
            check(!a.Connected()&&!b.Connected(),"clock mismatch");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            for(unsigned i=0;i<=LocalSciCable::capacity;++i)a.Send(true,std::uint8_t(i),100);
            check(a.GetState()==State::Error,"overflow must fail, not silently drop");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);
            a.Advance(100+q,1,0,cancelNow);check(a.GetState()==State::Error,"shutdown cancellation");
        }
        {
            LocalSciCable a(channel,q),b(channel,q);bind(a,b);b.Release();
            LocalSciCable replacement(channel,q);
            a.Advance(100+q,1,0,keepRunning);
            check(a.GetState()==State::Error,"new endpoint cannot inherit old session");
        }
    }
    exitedPeer(channel);
    std::cout<<"PASS shared SCI: seven quanta, 28000 ordered lane bytes, delivery, origins, clock/revision, overflow, cancellation, fresh sessions and abrupt peer exit\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
