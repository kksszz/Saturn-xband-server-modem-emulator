// Diagnostic only: ephemeral loopback port, no emulator or production server.
#include "host_fixture.hpp"
#include <xband/windows_modem_service.hpp>
#include <atomic>
#include <thread>
int rawProbe() {
    Pair pair;
    for(auto socket:{pair.client.value,pair.server.value}){
        u_long blocking=0;check(ioctlsocket(socket,FIONBIO,&blocking)==0,"blocking socket");
        const BOOL yes=TRUE;const DWORD timeout=2000;
        check(setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&yes),sizeof(yes))==0,"nodelay");
        check(setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))==0,"receive deadline");
        check(setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))==0,"send deadline");
    }
    const auto transfer=[](SOCKET socket,std::array<char,80>&bytes,bool write){
        size_t offset=0;while(offset<bytes.size()){
            const int n=write?send(socket,bytes.data()+offset,int(bytes.size()-offset),0):recv(socket,bytes.data()+offset,int(bytes.size()-offset),0);
            check(n>0,"raw transfer failed");offset+=size_t(n);
        }
    };
    std::atomic<bool> failed=false;
    std::jthread worker([&](std::stop_token stop){try{
        std::array<char,80> bytes{};for(unsigned i=0;i<2000&&!stop.stop_requested();++i){transfer(pair.server.value,bytes,false);transfer(pair.server.value,bytes,true);}
    }catch(...){failed=true;}});
    std::array<char,80> bytes{};const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<2000;++i){transfer(pair.client.value,bytes,true);transfer(pair.client.value,bytes,false);}
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    worker.join();check(!failed,"raw host failure");
    std::cout<<Json{{"mode","raw_tcp"},{"batches",2000},{"seconds",seconds},{"batches_per_second",2000/seconds}}.dump()<<'\n';return 0;
}
struct ProbeStats { uint64_t steps=0, waits=0, timeouts=0, waitNs=0; };
template<class Owner> void waitReady(Owner &owner, ProbeStats &stats, bool spin) {
    if(spin)return;
    fd_set rd,wr; FD_ZERO(&rd); FD_ZERO(&wr);
    if(!owner.appendWaitSockets(rd,wr))return;
    timeval timeout{0,1000};
    const auto start=std::chrono::steady_clock::now();
    const auto result=select(0,&rd,&wr,nullptr,&timeout);
    check(result!=SOCKET_ERROR,"select failed");
    ++stats.waits;if(!result)++stats.timeouts;
    stats.waitNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
}
int main(int argc,char **argv) {
    try {
        if(argc==2&&std::string_view(argv[1])=="raw")return rawProbe();
        const bool spin=argc==2&&std::string_view(argv[1])=="spin";
        Counts counts; Host host(configuration(),FrameClock(1,1000),[&](uint64_t){return std::make_unique<Echo>(counts);},Host::PortMode::loopback_ephemeral_test);
        const auto port=host.port();
        ProbeStats hs,cs;std::atomic<bool> failed=false;
        std::jthread worker([&](std::stop_token stop){try{while(!stop.stop_requested()){
            ++hs.steps;check(host.step(GetTickCount64()),"host stopped");waitReady(host,hs,spin);
        }}catch(...){failed=true;}});
        using Service=xband::windows::ModemServiceClient;
        Service client({"127.0.0.1",port,false,"test",std::string(64,'a'),1000},GetTickCount64());
        const auto pump=[&](auto done){
            const auto deadline=GetTickCount64()+5000;
            while(!done()){
                check(!failed&&GetTickCount64()<deadline,"probe timeout");
                ++cs.steps;check(client.step(GetTickCount64()),"client stopped");
                if(!done())waitReady(client,cs,spin);
            }
        };
        pump([&]{return client.state()==Service::State::idle;});
        check(client.open("0001",GetTickCount64()),"open");pump([&]{return client.carrier();});
        const auto start=std::chrono::steady_clock::now();
        std::array<uint8_t,80> bytes{}; unsigned completed=0;
        for(unsigned i=1;i<=2000;++i){
            check(client.transfer(bytes,uint64_t(i)*1000),"transfer");
            pump([&]{return client.readyToRun();});
            check(client.drain([](uint8_t b){check(b==0,"echo data");return true;},4096)==bytes.size(),"echo size");
            ++completed;
            if(std::chrono::steady_clock::now()-start>std::chrono::seconds(10))break;
        }
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        worker.request_stop();worker.join();
        std::cout<<Json{{"mode",spin?"spin":"select"},{"batches",completed},{"seconds",seconds},{"batches_per_second",completed/seconds},
            {"client_steps",cs.steps},{"client_waits",cs.waits},{"client_timeouts",cs.timeouts},{"client_wait_ms",cs.waitNs/1e6},
            {"host_steps",hs.steps},{"host_waits",hs.waits},{"host_timeouts",hs.timeouts},{"host_wait_ms",hs.waitNs/1e6}}.dump()<<'\n';
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
