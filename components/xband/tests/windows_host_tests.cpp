#include "host_fixture.hpp"
#include <xband/client_control.hpp>
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; Counts counts; auto config = configuration();
        auto factory = [&](uint64_t hz) -> std::unique_ptr<ServiceEndpoint> { check(hz == 1000, "guest clock"); return std::make_unique<Echo>(counts); };
        auto host = std::make_unique<Host>(config, FrameClock(1, 1000), factory, Host::PortMode::loopback_ephemeral_test);
        uint64_t time = 0;
        if (name == "host_roundtrip") {
            auto client = std::make_unique<Client>(host->port());
            client->request("hello", {{"client", "test"}, {"clock_hz", 1000}, {"auth_key", std::string(64, 'a')}}); client->exchange(*host, time, 1);
            client->request("open", {{"target", "local-service"}, {"subscriber", "0001"}}); client->exchange(*host, time, 2);
            const auto call = client->messages.back()["body"]["call"].get<std::string>();
            client->request("data", {{"call", call}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}}); client->exchange(*host, time, 3);
            check(host->step(++time), "settle completed input ACK");
            fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
            check(host->appendWaitSockets(readable,writable), "idle host can wait");
            client->request("advance", {{"call", call}, {"tick", "1000"}});
            bool localWork=false;
            for(unsigned i=0;i<1000&&!localWork;++i){
                if(const auto n=Harness::sendBytes(client->socket.value,client->outgoing.peek()))client->outgoing.consume(n);
                check(host->step(++time), "receive advance");
                FD_ZERO(&readable);FD_ZERO(&writable);
                localWork=!host->appendWaitSockets(readable,writable);
                if(!localWork)Sleep(1);
            }
            check(localWork, "accepted advance must drive before socket wait");
            client->exchange(*host, time, 6);
            check(client->messages[3]["body"]["payload_b64"] == "QUJD" && counts.input == 3, "host exact echo");
            client->request("close", {{"call", call}, {"reason", "hangup"}}); client->exchange(*host, time, 7);
            client->request("open", {{"target", "local-service"}, {"subscriber", "0001"}}); client->exchange(*host, time, 8);
            check(client->messages.back()["body"]["call"] != call, "new call token after reopen");
            check(shutdown(client->socket.value, SD_SEND) == 0, "EOF");
            for (unsigned i = 0; i < 1000 && host->clientCount(); ++i) { check(host->step(++time), "EOF pump"); Sleep(1); }
            check(host->clientCount() == 0 && counts.destroyed == 1, "EOF frees service");
        } else if (name == "host_client_adapter") {
            Client socket_client(host->port());
            ClientControl control("test", std::string(64, 'a'), 1000, time);
            const auto pump = [&](auto done) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                do {
                    check(control.poll(time), "client control poll");
                    if (const auto n = Harness::sendBytes(socket_client.socket.value, control.output())) check(control.sent(n), "client sent");
                    check(host->step(++time), "adapter host step");
                    std::array<uint8_t, 4096> bytes{};
                    const auto n = recv(socket_client.socket.value, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
                    if (n == SOCKET_ERROR) check(WSAGetLastError() == WSAEWOULDBLOCK, "adapter receive");
                    else {
                        check(n > 0, "adapter socket alive"); size_t cursor = 0;
                        while (cursor < static_cast<size_t>(n)) {
                            const auto result = control.feed(std::span(bytes).first(static_cast<size_t>(n)).subspan(cursor), time);
                            check(result.consumed && result.status != MessageInbox::Status::failed, "adapter dispatch"); cursor += result.consumed;
                        }
                    }
                    if (done()) return;
                    Sleep(1);
                } while (std::chrono::steady_clock::now() < deadline);
                throw std::runtime_error("adapter timeout");
            };
            pump([&] { return control.state() == ClientControl::State::idle; });
            check(control.open("003336666666", time) == Admission::queued, "adapter open");
            pump([&] { return control.state() == ClientControl::State::service; });
            const Json card{{"inserted", true}, {"read_state", "readable"}, {"number", "960500132270"}, {"remaining_units", 7}, {"nominal_units", 10}};
            check(control.snapshot(card, 0, 0, time) == Admission::queued, "adapter snapshot");
            pump([&] { return !host->status()["endpoints"][0]["peer_snapshot"].is_null(); });
            check(host->status()["endpoints"][0]["peer_snapshot"]["card"] == card, "card reaches host status through TCP");
            const std::array<uint8_t, 3> payload{0, 128, 255};
            check(control.transmit(payload, time) == Admission::queued && control.advance(1000, time) == Admission::queued, "TCP data and guest tick");
            pump([&] { return !control.advancing(); });
            check(std::equal(payload.begin(), payload.end(), control.received().begin(), control.received().end()) && counts.input == 3,
                "binary data through real TCP exactly once");
            check(control.consumeReceived(3, time), "TCP receive consumed");
            check(control.close(time) == Admission::queued, "adapter close");
            pump([&] { return control.state() == ClientControl::State::idle; });
            check(!host->status()["endpoints"][0]["call_active"].get<bool>(), "server call released");
            control.disconnect();
        } else if (name == "host_status") {
            check(host->status()["endpoints"][0]["connected"] == false, "configured endpoint initially offline");
            auto client = std::make_unique<Client>(host->port());
            // Client connect completion does not guarantee accept readiness in
            // the very next nonblocking host step on Windows.
            for (unsigned i = 0; i < 1000 && host->clientCount() == 0; ++i) {
                check(host->step(++time), "accept pending"); Sleep(1);
            }
            check(host->status()["pending_connections"] == 1 && host->status()["endpoints"][0]["connected"] == false,
                "unverified socket is not a connected modem");
            client->request("hello", {{"client", "test"}, {"clock_hz", 1000}, {"auth_key", std::string(64, 'a')}}); client->exchange(*host, time, 1);
            client->request("ping", Json::object()); client->exchange(*host, time, 2);
            check(host->status()["endpoints"][0]["connected"] == true && host->status()["pending_connections"] == 0, "accepted endpoint visible");
            Json body{{"call", nullptr}, {"subscriber", nullptr}, {"state", "idle"}, {"sent_bytes", "0"}, {"received_bytes", "0"},
                {"card", {{"inserted", false}, {"read_state", "absent"}, {"number", nullptr}, {"remaining_units", nullptr}, {"nominal_units", nullptr}}}};
            client->request("snapshot", body); client->request("ping", Json::object()); client->exchange(*host, time, 3);
            auto saved = host->status();
            check(saved["endpoints"][0]["peer_snapshot"] == body, "snapshot exported without mutation");
            check(saved.dump().find(std::string(64, 'a')) == std::string::npos && saved.dump().find(client->token_value) == std::string::npos,
                "keys and session token not exported");
            auto edited = saved; edited["endpoints"][0]["peer_snapshot"]["card"]["inserted"] = true;
            check(host->status() == saved, "consumer edit cannot modify live state");
            client->request("open", {{"target", "local-service"}, {"subscriber", "0001"}}); client->exchange(*host, time, 4);
            check(host->status()["endpoints"][0]["call_active"] == true && host->status()["endpoints"][0]["peer_snapshot"].is_null(), "new call has no stale status");
            check(shutdown(client->socket.value, SD_SEND) == 0, "status EOF");
            for (unsigned i = 0; i < 1000 && host->clientCount(); ++i) { check(host->step(++time), "status EOF pump"); Sleep(1); }
            check(host->clientCount() == 0 && host->status()["endpoints"][0]["connected"] == false, "disconnect returns configured row offline");
            check(saved["endpoints"][0]["peer_snapshot"] == body, "owned copy survives peer destruction");
            host->stop(); check(host->status()["running"] == false, "stop reported");
        } else if (name == "host_liveness") {
            auto client = std::make_unique<Client>(host->port());
            client->request("hello", {{"client", "test"}, {"clock_hz", 1000}, {"auth_key", std::string(64, 'a')}}); client->exchange(*host, time, 1);
            time = 3000; client->exchange(*host, time, 2);
            check(client->messages.back()["type"] == "ping", "idle TCP ping");
            client->request("pong", Json::object(), client->messages.back()["id"]);
            client->request("ping", Json::object()); client->exchange(*host, time, 3);
            check(client->messages.back()["type"] == "pong" && host->clientCount() == 1, "TCP heartbeat reply preserves connection");
            time += 2001; client->exchange(*host, time, 4); check(client->messages.back()["type"] == "ping", "second ping");
            check(host->step(time + 5000) && host->clientCount() == 0 && counts.destroyed == 1, "unanswered TCP ping disposes peer");
        } else if (name == "host_exclusive") {
            config.port = host->port(); bool rejected = false;
            try { Host duplicate(config, FrameClock(1, 1000), factory); } catch (const std::runtime_error &) { rejected = true; }
            check(rejected, "cannot share active listener"); host->stop();
            Host replacement(config, FrameClock(1, 1000), factory); check(replacement.port() == config.port, "port released on stop");
        } else if (name == "host_bounds") {
            std::vector<std::unique_ptr<Client>> clients;
            for (unsigned i = 0; i < 33; ++i) { clients.push_back(std::make_unique<Client>(host->port())); check(host->step(++time), "accept bounded"); }
            check(host->clientCount() == Host::max_clients && counts.resets == 0, "32 unauthenticated limit, no service");
            check(host->step(time + ConnectionLifecycle::handshake_ms), "timeout step");
            check(host->clientCount() == 0, "all unverified peers expire");
            check(!host->step(0) && host->stopped(), "clock reversal closes listener");
        } else if (name == "host_bind_policy") {
            for (const auto *address : {"0.0.0.0", "8.8.8.8"}) {
                config.bind_address = address; config.allow_lan = true; bool rejected = false;
                try { Host bad(config, FrameClock(1, 1000), factory); } catch (const std::invalid_argument &) { rejected = true; }
                check(rejected, "unsafe configuration never binds");
            }
            IN_ADDR zero{}; check(!xband::windows::localUnicast(zero), "unspecified not local unicast");
        } else throw std::runtime_error("unknown test");
        host->stop(); check(host->stopped() && host->clientCount() == 0, "stop complete");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
