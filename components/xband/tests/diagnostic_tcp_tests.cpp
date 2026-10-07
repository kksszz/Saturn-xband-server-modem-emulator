#include "authenticated_fixture.hpp"
#include <local_service_endpoint.hpp>
using Bytes = std::vector<uint8_t>;
Bytes packet(uint16_t protocol, uint8_t code, uint8_t id, Bytes options = {}) {
    const auto length = options.size() + 4;
    Bytes bytes{0xff, 3, static_cast<uint8_t>(protocol >> 8), static_cast<uint8_t>(protocol), code, id,
        static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
    bytes.insert(bytes.end(), options.begin(), options.end()); return LocalPPPProbe::encode(bytes);
}
struct Diagnostic {
    // Borrowed parser must outlive both its adapter and connection host.
    LocalPPPProbe reference, routed;
    EndpointRegistry registry{{{"test", configured_key}}};
    Connection wire{registry, false, [this](uint64_t) { return std::make_unique<LocalServiceEndpoint>(routed); }};
    uint64_t input_offset = 0, output_offset = 0;
    explicit Diagnostic(bool ipcp = false) {
        reference.enableIPCP = routed.enableIPCP = ipcp;
        wire.hello(); wire.replies(1);
        wire.request("open", {{"target", "local-service"}, {"subscriber", "0001"}}); wire.replies(2);
    }
    void batch(const Bytes &bytes, unsigned frame) {
        for (const auto byte : bytes) reference.feed(byte, frame);
        reference.tick(frame); const auto expected = reference.out; reference.out.clear();
        if (!bytes.empty()) {
            std::string encoded; check(encodeBase64(bytes, encoded), "encode PPP input");
            const auto count = wire.messages.size();
            wire.request("data", {{"call", wire.call_token}, {"offset", std::to_string(input_offset)},
                {"bytes", bytes.size()}, {"payload_b64", encoded}});
            input_offset += bytes.size(); wire.replies(count + 1);
            check(wire.messages.back()["type"] == "data_ack" &&
                wire.messages.back()["body"]["next_offset"] == std::to_string(input_offset), "receipt ACK exact");
        }
        const auto start = wire.messages.size();
        const auto request_id = wire.request("advance", {{"call", wire.call_token}, {"tick", std::to_string(uint64_t{frame} * 1000)}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool completed = false; size_t cursor = start; Bytes actual;
        while (!completed && std::chrono::steady_clock::now() < deadline) {
            wire.step();
            while (cursor < wire.messages.size()) {
                const auto &reply = wire.messages[cursor++]; check(!completed, "ordered completion last");
                if (reply["type"] == "data") {
                    const auto &body = reply["body"];
                    check(body["call"].get_ref<const std::string &>() == wire.call_token && body["offset"] == std::to_string(output_offset), "PPP output call/offset");
                    std::array<uint8_t, 4096> decoded{}; size_t count = 0;
                    check(decodeBase64(body["payload_b64"].get_ref<const std::string &>(), decoded, count), "decode PPP reply");
                    actual.insert(actual.end(), decoded.begin(), decoded.begin() + count); output_offset += count;
                } else if (reply["type"] == "advance_ok") {
                    check(reply["reply_to"] == std::to_string(request_id) && reply["body"]["tick"] == std::to_string(uint64_t{frame} * 1000), "completion correlation/time");
                    completed = true;
                } else check(reply["type"] == "data_ack", "expected ACK");
            }
            if (!completed) Sleep(1);
        }
        check(completed && actual == expected, "TCP PPP byte parity");
        check(reference.valid == routed.valid && reference.invalid == routed.invalid && reference.open() == routed.open() &&
            reference.tries == routed.tries && reference.lastRequest == routed.lastRequest && reference.stopped == routed.stopped &&
            reference.ipOpen() == routed.ipOpen() && reference.ipTries == routed.ipTries && reference.ipStopped == routed.ipStopped &&
            reference.frame == routed.frame && reference.escaped == routed.escaped, "TCP PPP state parity");
        if (output_offset) wire.request("data_ack", {{"call", wire.call_token}, {"next_offset", std::to_string(output_offset)},
            {"limit", std::to_string(output_offset + 65536)}});
    }
    void reopen() {
        auto count = wire.messages.size(); wire.request("close", {{"call", wire.call_token}, {"reason", "reset"}});
        wire.replies(++count); check(wire.messages.back()["type"] == "close_ok", "PPP close");
        const auto previous_call = wire.call_token;
        wire.request("open", {{"target", "local-service"}, {"subscriber", "9876543210"}});
        wire.replies(++count); check(wire.messages.back()["type"] == "open_ok" && wire.call_token != previous_call, "PPP reopen uses new call ID");
        reference.resetSession(); input_offset = output_offset = 0;
    }
};
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1];
        auto h = std::make_unique<Diagnostic>(name == "tcp_ppp_ipcp");
        if (name == "tcp_ppp_lcp") {
            h->batch(packet(0xc021, 1, 1), 0); h->batch(packet(0xc021, 2, 0x51), 1);
            check(h->routed.open(), "TCP LCP open");
            h->batch(packet(0xc021, 5, 2), 2); check(h->routed.stopped, "TCP termination");
        } else if (name == "tcp_ppp_ipcp") {
            h->batch(packet(0xc021, 1, 1), 0); h->batch(packet(0xc021, 2, 0x51), 1);
            h->batch(packet(0x8021, 1, 2, {3, 6, 0, 0, 0, 0}), 2);
            h->batch(packet(0x8021, 1, 3, {3, 6, 10, 0, 0, 2}), 3);
            h->batch(packet(0x8021, 2, 0x61, {3, 6, 10, 0, 0, 1}), 4);
            check(h->routed.ipOpen(), "TCP IPCP open");
        } else if (name == "tcp_ppp_retry") {
            h->batch(packet(0xc021, 1, 1), 0);
            for (unsigned frame : {179u, 180u, 359u, 360u, 540u, 720u, 899u, 900u}) h->batch({}, frame);
            check(h->routed.tries == 5 && h->routed.stopped, "TCP retry and stop");
        } else if (name == "tcp_ppp_reset") {
            h->batch(Bytes{0x7e, 0xff, 0x55, 0x66, 0x77, 0x88, 0x99, 0x7e}, 0);
            check(h->routed.invalid != 0, "TCP bad FCS rejected");
            h->batch(Bytes{0x7e, 0x7d}, 1); check(h->routed.escaped, "incomplete escape retained");
            h->reopen(); check(!h->routed.escaped, "escape cleared by close/open");
            auto escaped = packet(0xc021, 1, 0x7e, {5, 6, 0x7d, 0x7e, 0, 1});
            const auto escape = std::find(escaped.begin(), escaped.end(), uint8_t{0x7d});
            check(escape != escaped.end(), "escape exists"); const auto split = escape + 1;
            check(split != escaped.end(), "escaped fixture split");
            h->batch(Bytes(escaped.begin(), split), 0);
            h->batch(Bytes(split, escaped.end()), 0);
            check(h->routed.valid != 0, "fragmented PPP accepted after reset");
        } else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
