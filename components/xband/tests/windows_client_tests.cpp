#include "host_fixture.hpp"
#include <xband/windows_tcp_client.hpp>
#include <xband/remote_service_batch.hpp>
#include <xband/staged_service_batch.hpp>
#include <xband/modem_call_control.hpp>
#include <xband/windows_modem_service.hpp>
using TcpClient = xband::windows::TcpClient;
using ClientConfig = xband::windows::ClientConfig;
ClientConfig clientConfig(uint16_t port) { return {"127.0.0.1", port, false, "test", std::string(64, 'a'), 1000}; }
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; Counts counts; uint64_t time = 0;
        Host host(configuration(), FrameClock(1, 1000), [&](uint64_t hz) {
            check(hz == 1000, "clock"); return std::make_unique<Echo>(counts);
        }, Host::PortMode::loopback_ephemeral_test);
        auto config = clientConfig(host.port());
        if (name == "client_socket_policy") {
            check(!xband::windows::permittedServer("192.168.1.2", false) && xband::windows::permittedServer("192.168.1.2", true), "LAN explicit opt in");
            for (const auto *address : {"0.0.0.0", "8.8.8.8", "localhost", "127.0.0.2", "192.168.001.2"}) {
                config.server_address = address; config.allow_lan = true; bool rejected = false;
                try { TcpClient bad(config, 0); } catch (const std::invalid_argument &) { rejected = true; }
                check(rejected, "invalid address rejected before connect");
            }
        } else if(name=="modem_service_deferred_timeout") {
            using Service=xband::windows::ModemServiceClient;
            Service service(config,time);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(service.state()!=Service::State::idle){
                check(std::chrono::steady_clock::now()<deadline,"timeout-test handshake bounded");
                check(host.step(++time)&&service.step(time),"timeout-test handshake");
                Sleep(1);
            }
            check(service.open("0001",time)&&service.requestClose(time),"opening close request queued");
            // Do not pump the host: the original open cannot receive open_ok.
            check(!service.step(time+ConnectionLifecycle::request_ms),"deferred close retains original request deadline");
            check(service.state()==Service::State::stopped&&service.transportFailure()==TcpClient::Failure::protocol,
                "deadline remains a fatal transport/protocol failure");
            check(!service.closePending()&&!service.open("0001",time+ConnectionLifecycle::request_ms),"timeout does not retry or reopen");
        } else if(name=="modem_service_deferred_close"||name=="modem_service_deferred_loss") {
            using Service=xband::windows::ModemServiceClient;
            Service service(config,time);
            const auto pump=[&](auto done){
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                do{
                    check(host.step(++time),"deferred-close host step");
                    check(service.step(time),"deferred-close service step");
                    if(done())return;
                    Sleep(1);
                }while(std::chrono::steady_clock::now()<deadline);
                throw std::runtime_error("deferred-close timeout");
            };
            check(service.requestClose(time)&&service.closePending(),"reset during TCP/hello completion is deferred");
            pump([&]{return service.state()==Service::State::idle;});
            check(!service.closePending()&&counts.resets==0,"startup reset creates no service call");
            check(service.open("0001",time),"original open queued");
            check(service.requestClose(time)&&service.closePending()&&!service.carrier(),"close waits for old open without early carrier");
            check(!service.open("0001",time),"new open blocked until old close ack");
            pump([&]{return service.state()==Service::State::idle;});
            check(!service.closePending()&&service.pending()==0&&counts.input==0,"old open completes and closes without payload");
            check(service.open("0001",time),"fresh open after deferred close");
            pump([&]{return service.carrier();});
            const std::array<uint8_t,3> oldBytes{11,22,33};
            check(service.transfer(oldBytes,1000),"old transfer pending");
            check(service.requestClose(time)&&service.closePending()&&!service.carrier()&&!service.readyToRun(),"reset hides old carrier and responses immediately");
            check(service.requestClose(time)&&!service.transfer(oldBytes,1001)&&!service.open("0001",time),"repeated reset admits no duplicate work");
            if(name=="modem_service_deferred_loss"){
                check(!service.step(time-1)&&service.state()==Service::State::stopped,"fault during deferred close remains fatal");
                check(service.failure()==Service::Failure::transport&&!service.open("0001",time),"no automatic reconnect after deferred-close fault");
            }else{
                pump([&]{return service.state()==Service::State::idle;});
                check(counts.input==oldBytes.size()&&service.pending()==0&&!service.closePending(),"already submitted work finishes once, stale responses discarded");
                check(service.drain([](uint8_t)->bool{throw std::runtime_error("stale response reached guest");})==0,"old response cannot reach guest");
                check(service.open("0001",time),"next dial accepted after close ack");
                pump([&]{return service.carrier();});
                const std::array<uint8_t,1> newBytes{99};
                check(service.transfer(newBytes,0),"new call accepts independent guest tick");
                pump([&]{return service.readyToRun();});
                std::vector<uint8_t> received;
                service.drain([&](uint8_t byte){received.push_back(byte);return true;});
                check(received==std::vector<uint8_t>{99}&&counts.input==4,"only new response delivered, no old input replay");
                check(service.requestClose(time),"normal completed call close");
                pump([&]{return service.state()==Service::State::idle;});
                check(service.failure()==Service::Failure::none,"deferred close never marks connection failed");
            }
        } else if(name=="modem_service_redial_cycles") {
            using Service=xband::windows::ModemServiceClient;
            Service service(config,time);
            const auto pump=[&](auto done){
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                do{
                    check(host.step(++time),"redial host step");
                    check(service.step(time),"redial service step");
                    if(done())return;
                    Sleep(1);
                }while(std::chrono::steady_clock::now()<deadline);
                throw std::runtime_error("redial cycle timeout");
            };
            pump([&]{return service.state()==Service::State::idle;});
            for(unsigned cycle=0;cycle<32;++cycle){
                check(service.open("0001",time),"repeated service open");
                pump([&]{return service.carrier();});
                const std::array<uint8_t,3> bytes{uint8_t(cycle),0,255};
                check(service.transfer(bytes,1000),"repeated transfer");
                pump([&]{return service.readyToRun();});
                check(service.pending()==bytes.size(),"old responses never leak into new call");
                // Closing after a completed network batch may discard unread
                // staging. Alternate with normal UART consumption.
                if(cycle%2){
                    std::vector<uint8_t> received;
                    service.drain([&](uint8_t byte){received.push_back(byte);return true;});
                    check(std::equal(received.begin(),received.end(),bytes.begin(),bytes.end()),"cycle echo exact");
                }
                check(service.close(time),"repeated close queued");
                check(!service.open("0001",time),"redial must wait for close acknowledgement");
                pump([&]{return service.state()==Service::State::idle;});
                check(!service.carrier()&&service.pending()==0&&service.failure()==Service::Failure::none,"close leaves clean reusable connection");
            }
        } else if(name=="modem_service_owner"||name=="modem_service_stop"||name=="modem_service_loss") {
            using Service=xband::windows::ModemServiceClient;
            Service service(config,time);
            const auto pump=[&](auto done){
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                do{
                    check(host.step(++time),"service host step");
                    check(service.step(time),"service owner step");
                    if(done())return;
                    Sleep(1);
                }while(std::chrono::steady_clock::now()<deadline);
                throw std::runtime_error("service owner timeout");
            };
            check(!service.carrier()&&!service.transfer({},0),"no transfer before connection");
            pump([&]{return service.state()==Service::State::idle;});
            check(service.open("0001",time)&&!service.carrier(),"open queued, no early carrier");
            if(name=="modem_service_stop"){
                check(!service.close(time)&&service.state()==Service::State::stopped,"cancel pending open");
                check(service.failure()==Service::Failure::close_busy&&service.failureState()==Service::State::opening,
                    "pending-open failure retains diagnostic state");
                check(std::string_view(service.failureName())=="close attempted during pending work","local failure is not mislabeled as server disconnect");
                check(!service.step(++time)&&!service.carrier()&&!service.readyToRun(),"no stale connect after stop");
                service.stop();service.stop();
            }else{
                pump([&]{return service.carrier();});
                std::array<uint8_t,400> bytes{};
                for(size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<uint8_t>(i);
                const auto expected=bytes;
                check(service.transfer(bytes,1000)&&!service.readyToRun(),"batch holds guest boundary");
                fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
                check(!service.appendWaitSockets(readable,writable),"staged batch must run before socket wait");
                bytes.fill(0);
                check(service.drain([](uint8_t)->bool{throw std::runtime_error("early drain");})==0,"no partial response");
                pump([&]{return service.readyToRun();});
                check(service.pending()==400,"bounded response retained");
                check(service.drain([](uint8_t){return false;})==0&&service.pending()==400,"full UART retains bytes");
                std::vector<uint8_t> received;
                while(service.pending())service.drain([&](uint8_t b){received.push_back(b);return true;},17);
                check(std::equal(received.begin(),received.end(),expected.begin(),expected.end()),"snapshot and ordered drain");
                check(service.close(time)&&!service.carrier(),"close immediately drops carrier");
                pump([&]{return service.state()==Service::State::idle;});
                check(service.open("0001",time),"redial");
                pump([&]{return service.carrier();});
                check(service.transfer(bytes,2000),"pending batch");
                if(name=="modem_service_loss"){
                    check(!service.step(time-1),"transport clock fault destroys control with pending batch");
                    check(service.failure()==Service::Failure::transport&&service.failureState()==Service::State::transferring&&
                        service.transportFailure()==TcpClient::Failure::clock_reversed,"transport failure retains source and previous state");
                    check(std::string_view(service.failureName())=="host clock reversed","transport diagnostic reports actual category");
                }
                else service.stop();
                check(!service.step(++time)&&service.pending()==0&&!service.carrier(),"shutdown cancels pending work");
            }
        } else {
            if (name == "client_socket_denied") config.key = std::string(64, 'b');
            TcpClient client(config, 0);
            const auto pump = [&](auto done) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    check(host.step(++time), "server step");
                    client.step(time);
                    if (client.stopped() && name != "client_socket_denied") throw std::runtime_error("unexpected client stop");
                    if (done()) return;
                    check(!client.stopped(), "unexpected client stop");
                    Sleep(1);
                } while (std::chrono::steady_clock::now() < deadline);
                throw std::runtime_error("test pump timeout");
            };
            if (name == "client_socket_denied") {
                pump([&] { return client.stopped(); });
                check(client.control() == nullptr && counts.resets == 0, "bad key creates no service");
            } else if (name == "client_modem_call" || name == "client_modem_cancel") {
                pump([&] { return client.control() && client.control()->state() == ClientControl::State::idle; });
                ModemCallControl modem(*client.control(), "0001", "5550100");
                check(modem.command("ATDT999", time) && modem.reply() == "\r\nNO CARRIER\r\n" &&
                    client.control()->state() == ClientControl::State::idle, "unknown number never routed");
                modem.consume(modem.reply().size());
                check(modem.command("ATS91=15S92=15DT5550100", time) && !modem.carrier() && modem.reply().empty(), "dial waits for open ack");
                if (name == "client_modem_cancel") {
                    check(modem.command("ATH0", time), "cancel pending dial");
                    check(modem.state() == ModemCallControl::State::stopped && !modem.carrier() &&
                        client.control()->state() == ClientControl::State::stopped, "pending dial cannot later connect");
                    modem.poll(); check(modem.reply() == "\r\nOK\r\n", "no stale CONNECT after cancellation");
                } else {
                    pump([&] { modem.poll(); return modem.carrier(); });
                    check(modem.reply() == "\r\nCONNECT 14400\r\n", "single connected result");
                    modem.consume(modem.reply().size()); const auto old = std::string(client.control()->call());
                    check(modem.command("ATH0", time) && !modem.carrier(), "hangup drops DCD immediately");
                    pump([&] { modem.poll(); return modem.state() == ModemCallControl::State::idle; });
                    check(modem.reply() == "\r\nOK\r\n", "close ack result"); modem.consume(modem.reply().size());
                    check(modem.command("ATDT5550100", time), "redial");
                    pump([&] { modem.poll(); return modem.carrier(); });
                    check(client.control()->call() != old, "redial gets new identity");
                    check(client.control()->advance(0, time) == Admission::queued, "pending transfer");
                    check(modem.command("ATZ", time) && modem.state() == ModemCallControl::State::stopped && !modem.carrier(), "reset cancels transfer, never replays");
                }
            } else if (name == "client_staging_resume" || name == "client_staging_overflow") {
                pump([&] { return client.control() && client.control()->state() == ClientControl::State::idle; });
                check(client.control()->open("0001", time) == Admission::queued, "staging open");
                pump([&] { return client.control()->state() == ClientControl::State::service; });
                StagedServiceBatch<512> batch(*client.control());
                std::vector<uint8_t> input(name == "client_staging_overflow" ? 513 : 400);
                for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>(i);
                check(batch.begin(input, 1000) && !batch.readyToRun(), "hold during network batch");
                bool failed = false;
                pump([&] {
                    check(batch.drain([](uint8_t) -> bool { throw std::runtime_error("early delivery"); }) == 0, "no partial delivery");
                    const auto result = batch.resume(time, 31);
                    failed = result == RemoteServiceBatch::Step::failed;
                    return failed || result == RemoteServiceBatch::Step::done;
                });
                if (name == "client_staging_overflow") {
                    check(failed && batch.failed() && batch.pending() == 0 && !batch.readyToRun(), "overflow clears staged bytes and never grants execution");
                    check(client.control()->state() == ClientControl::State::stopped, "overflow disconnects instead of waiting forever");
                } else {
                    check(!failed && batch.readyToRun() && batch.pending() == 400, "complete before UART drain");
                    check(batch.drain([](uint8_t) { return false; }) == 0 && batch.readyToRun(), "full UART still permits guest run");
                    std::vector<uint8_t> received;
                    check(batch.drain([&](uint8_t byte) { received.push_back(byte); return true; }, 300) == 300, "bounded drain");
                    check(batch.begin(input, 2000), "retain backlog across next boundary");
                    pump([&] { const auto r = batch.resume(time); check(r != RemoteServiceBatch::Step::failed, "ring alive"); return r == RemoteServiceBatch::Step::done; });
                    while (batch.pending()) batch.drain([&](uint8_t byte) { received.push_back(byte); return true; }, 17);
                    auto expected = input; expected.insert(expected.end(), input.begin(), input.end());
                    check(received == expected, "ring wrap preserves backlog order");
                    check(batch.begin(input, 3000), "cancel pending batch"); batch.cancel();
                    check(batch.failed() && batch.pending() == 0 && !batch.readyToRun(), "cancel invalidates permission");
                }
            } else if (name == "client_batch_roundtrip" || name == "client_batch_cancel") {
                pump([&] { return client.control() && client.control()->state() == ClientControl::State::idle; });
                check(client.control()->open("0001", time) == Admission::queued, "batch open");
                pump([&] { return client.control()->state() == ClientControl::State::service; });
                RemoteServiceBatch batch(*client.control());
                std::array<uint8_t, 5000> bytes{};
                for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i);
                const auto original = bytes;
                check(batch.begin(bytes, 2000) && !batch.begin(bytes, 3000), "one batch at a time");
                bytes.fill(0); // queued batch owns its input snapshot
                if (name == "client_batch_cancel") {
                    check(batch.resume(time, [](uint8_t) { return true; }) == RemoteServiceBatch::Step::progress, "partially queued");
                    batch.cancel();
                    check(!batch.busy() && batch.resume(time, [](uint8_t) { return true; }) == RemoteServiceBatch::Step::failed,
                        "cancel cannot replay");
                    check(client.control()->state() == ClientControl::State::stopped && client.control()->output().empty(), "cancel clears connection queue");
                } else {
                    std::vector<uint8_t> result;
                    bool accept_output = false, blocked_output_seen = false;
                    RemoteServiceBatch::Step state = RemoteServiceBatch::Step::idle;
                    pump([&] {
                        state = batch.resume(time, [&](uint8_t byte) {
                            if (!accept_output) { blocked_output_seen = true; return false; }
                            result.push_back(byte); return true;
                        }, 1024);
                        check(state != RemoteServiceBatch::Step::failed, "batch alive");
                        if (blocked_output_seen) accept_output = true;
                        return state == RemoteServiceBatch::Step::done;
                    });
                    check(blocked_output_seen && std::equal(original.begin(), original.end(), result.begin(), result.end()), "copy and sink backpressure preserve bytes");
                    check(counts.input == original.size() && counts.ticks == 1 && !batch.busy(), "no repeated transmit or guest tick");
                    check(batch.resume(time, [](uint8_t) -> bool { throw std::runtime_error("duplicate delivery"); }) == RemoteServiceBatch::Step::idle,
                        "completed batch does not run again");
                }
            } else if (name == "client_socket_roundtrip") {
                pump([&] { return client.control() && client.control()->state() == ClientControl::State::idle; });
                check(client.control()->open("003336666666", time) == Admission::queued, "open");
                pump([&] { return client.control()->state() == ClientControl::State::service; });
                std::array<uint8_t,4096> data{}; for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
                check(client.control()->transmit(data, time) == Admission::queued && client.control()->advance(1000, time) == Admission::queued, "data and tick");
                pump([&] { return !client.control()->advancing(); });
                const auto received = client.control()->received();
                check(std::equal(data.begin(), data.end(), received.begin(), received.end()) && counts.input == data.size(), "exact binary roundtrip");
                check(client.control()->consumeReceived(data.size(), time), "consume guest data");
                check(client.control()->close(time) == Admission::queued, "hangup");
                pump([&] { return client.control()->state() == ClientControl::State::idle; });
                time += 2001;
                for (unsigned i = 0; i < 500; ++i) { check(host.step(++time) && client.step(time), "idle heartbeat"); Sleep(1); }
                host.stop();
                for (unsigned i = 0; i < 1000 && !client.stopped(); ++i) { client.step(++time); Sleep(1); }
                check(client.stopped() && client.control() == nullptr, "EOF discards control and buffers");
            } else if (name == "client_socket_clock") {
                pump([&] { return client.control() && client.control()->state() == ClientControl::State::idle; });
                check(!client.step(0) && client.failure() == TcpClient::Failure::clock_reversed && !client.control(), "reverse host time closes");
            } else if (name == "client_socket_handshake_timeout") {
                // Accept TCP at OS level but never run host application handshake.
                for (unsigned i = 0; i < 1000 && client.connecting(); ++i) { client.step(++time); Sleep(1); }
                check(client.control() != nullptr, "TCP connected");
                check(!client.step(time + 5000) && client.failure() == TcpClient::Failure::protocol, "missing hello reply expires");
            } else throw std::runtime_error("unknown test");
        }
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
