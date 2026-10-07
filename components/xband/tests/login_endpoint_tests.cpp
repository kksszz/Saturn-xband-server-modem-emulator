#include "host_fixture.hpp"
#include "../tools/diagnostic_login_endpoint.hpp"
#include <xband/windows_tcp_client.hpp>
#include <xband/staged_service_batch.hpp>
using Bytes = std::vector<uint8_t>;
Bytes drain(DiagnosticLoginEndpoint &endpoint) {
    Bytes result; uint8_t byte;
    while (endpoint.peek(byte)) { result.push_back(byte); endpoint.consume(); }
    return result;
}
int main(int argc,char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1];
        if (name == "login_tcp") {
            Host host(configuration(), FrameClock(60,1000000000), [](uint64_t hz)->std::unique_ptr<ServiceEndpoint> {
                check(hz==1000000000,"clock"); return std::make_unique<DiagnosticLoginEndpoint>();
            },Host::PortMode::loopback_ephemeral_test);
            uint64_t now=0;
            xband::windows::TcpClient client({"127.0.0.1",host.port(),false,"test",std::string(64,'a'),1000000000},now);
            const auto pump=[&](auto done) {
                const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                do { check(host.step(++now)&&client.step(now),"TCP alive"); if(done())return; Sleep(1); }
                while(std::chrono::steady_clock::now()<end);
                throw std::runtime_error("login TCP timeout");
            };
            pump([&]{return client.control()&&client.control()->state()==ClientControl::State::idle;});
            check(client.control()->open("0001",now)==Admission::queued,"open");
            pump([&]{return client.control()->state()==ClientControl::State::service;});
            StagedServiceBatch<> batch(*client.control());
            const auto exchange=[&](const Bytes &input,uint64_t tick) {
                check(batch.begin(input,tick),"begin");
                pump([&]{const auto r=batch.resume(now,3);check(r!=RemoteServiceBatch::Step::failed,"batch");return r==RemoteServiceBatch::Step::done;});
                Bytes result;
                while(batch.pending())batch.drain([&](uint8_t byte){result.push_back(byte);return true;},1);
                return result;
            };
            check(exchange({'\r'},0)==Bytes({'H','E','L','O'}),"HELO");
            check(exchange({},999999999).empty(),"guest-time guard not host-time");
            check(exchange({},1000000000)==Bytes({'l','o','g','i','n',':'}),"login at 60 frames");
            check(exchange({'P','s','t'},1000000000).empty(),"partial username");
            check(exchange({'r','n','\n'},1000000000)==Bytes({'P','a','s','s','w','o','r','d',':'}),"password prompt");
            // Synthetic credential, never taken from a user's capture.
            Bytes input{'t','e','s','t','p','a','s','s','\n'};
            auto lcp=LocalPPPProbe::encode({0xff,3,0xc0,0x21,1,1,0,4});
            input.insert(input.end(),lcp.begin(),lcp.end());
            LocalPPPProbe reference;reference.enableIPCP=true;
            for(auto b:lcp)reference.feed(b,60);reference.tick(60);
            check(exchange(input,1000000000)==reference.out,"password and PPP same batch preserve frame");
            client.stop();host.stop();
        } else {
            DiagnosticLoginEndpoint endpoint;
            if(name=="login_reject") {
                for(unsigned scenario=0;scenario<4;++scenario) {
                    endpoint.reset();bool rejected=false;
                    try {
                        if(scenario==0)endpoint.transmit(0x7e,0);
                        endpoint.transmit('\r',0);endpoint.tick(60);
                        if(scenario==1)endpoint.transmit('X',60);
                        for(auto b:std::string_view("Pstrn\n"))endpoint.transmit(static_cast<uint8_t>(b),60);
                        if(scenario==2)endpoint.transmit('\n',60);
                        else endpoint.tick(59);
                    } catch(const std::runtime_error &e) {
                        rejected=std::string_view(e.what())=="diagnostic login profile mismatch";
                    }
                    check(rejected&&endpoint.phase()==DiagnosticLoginEndpoint::Phase::failed&&endpoint.pending()==0,"fail closed without secret diagnostics");
                }
            } else if(name=="login_reset") {
                endpoint.transmit('\r',12);endpoint.tick(71);check(endpoint.phase()==DiagnosticLoginEndpoint::Phase::greeting,"relative guard");
                endpoint.tick(72);const auto out=drain(endpoint);
                check(std::string(out.begin(),out.end())=="HELOlogin:","queued prompts keep order");
                endpoint.transmit('P',72);endpoint.reset();
                check(endpoint.pending()==0&&endpoint.phase()==DiagnosticLoginEndpoint::Phase::initial,"reset clears partial login");
                endpoint.transmit('\r',0);check(drain(endpoint)==Bytes({'H','E','L','O'}),"fresh origin");
            } else throw std::runtime_error("unknown test");
        }
        std::cout<<"PASS "<<name<<'\n';return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
