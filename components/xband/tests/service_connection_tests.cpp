#include "loopback_fixture.hpp"
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; auto h = std::make_unique<Harness>(); h->open();
        if (name == "tcp_roundtrip") {
            h->request("data", {{"call", call1}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}}); h->until(2);
            check(h->counts.input == 0, "input staged until advance");
            h->request("advance", {{"call", call1}, {"tick", "1000"}}); h->until(5);
            check(h->messages[2]["type"] == "data" && h->messages[2]["body"]["payload_b64"] == "QUJD" &&
                h->messages[4]["type"] == "advance_ok" && h->counts.input == 3 && h->counts.ticks == 1, "TCP echo exact");
            h->request("close", {{"call", call1}, {"reason", "hangup"}}); h->until(6);
            check(h->host.call().empty() && h->messages[5]["type"] == "close_ok", "close reply");
            h->fresh_call = call2; h->request("open", {{"target", "local-service"}, {"subscriber", "9876543210"}}); h->until(7);
            h->request("advance", {{"call", call2}, {"tick", "0"}}); h->until(8);
            check(h->messages[7]["type"] == "advance_ok" && h->host.call() == call2 && h->counts.input == 3 && h->counts.resets == 3, "new call fresh clock and no replay");
        } else if (name == "tcp_cancel_staged") {
            h->request("data", {{"call", call1}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}}); h->until(2);
            h->request("close", {{"call", call1}, {"reason", "reset"}}); h->until(3);
            h->fresh_call = call2; h->request("open", {{"target", "local-service"}, {"subscriber", "9876543210"}}); h->until(4);
            h->request("data", {{"call", call2}, {"offset", "0"}, {"bytes", 1}, {"payload_b64", "RA=="}}); h->until(5);
            h->request("advance", {{"call", call2}, {"tick", "0"}}); h->until(8);
            check(h->counts.input == 1 && h->counts.ticks == 1 && h->messages[5]["body"]["payload_b64"] == "RA==" &&
                h->messages[5]["body"]["offset"] == "0" && h->messages[7]["type"] == "advance_ok", "close cancels ABC; fresh call returns only D at offset zero");
        } else if (name == "tcp_disconnect") {
            h->request("data", {{"call", call1}, {"offset", "0"}, {"bytes", 1}, {"payload_b64", "QQ=="}}); h->until(2);
            check(shutdown(h->sockets.client.value, SD_SEND) == 0, "client half-close");
            ready(h->sockets.server.value, false); char byte = 0;
            check(recv(h->sockets.server.value, &byte, 1, 0) == 0, "real socket EOF"); h->host.disconnect();
            check(h->host.output().empty() && h->host.call().empty() && h->counts.destroyed == 1 && h->counts.input == 0, "EOF cancels staged data");
            check(h->host.poll(h->now) == ServiceConnection::Outcome::failed, "closed cannot restart");
        } else if (name == "connection_timeout") {
            check(h->host.poll(h->now + ConnectionLifecycle::idle_ms) == ServiceConnection::Outcome::failed && h->counts.destroyed == 1 && h->host.output().empty(), "timeout disposes service");
        } else if (name == "connection_generation") {
            const auto newer = activate(h->lifecycle); check(newer != h->generation, "new generation"); h->host.disconnect();
            check(h->lifecycle.state() == ConnectionLifecycle::State::active && h->counts.destroyed == 1, "old close cannot close new connection");
        } else throw std::runtime_error("Unknown case");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
