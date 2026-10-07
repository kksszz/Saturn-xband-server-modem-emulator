#include "host_fixture.hpp"
#include "../tools/diagnostic_endpoint.hpp"
using Bytes = std::vector<uint8_t>;
Bytes packet(uint16_t protocol, uint8_t code, uint8_t id, Bytes options = {}) {
    const auto size = options.size() + 4;
    Bytes bytes{0xff, 3, static_cast<uint8_t>(protocol >> 8), static_cast<uint8_t>(protocol), code, id,
        static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
    bytes.insert(bytes.end(), options.begin(), options.end()); return LocalPPPProbe::encode(bytes);
}
struct Test {
    LocalPPPProbe reference;
    Host host{configuration(), FrameClock(1, 1000), [](uint64_t hz) -> std::unique_ptr<ServiceEndpoint> {
        check(hz == 1000, "diagnostic clock"); return std::make_unique<OwnedDiagnosticEndpoint>();
    }, Host::PortMode::loopback_ephemeral_test};
    Client client{host.port()}; uint64_t time = 0, offset = 0, output = 0; std::string call;
    Test() {
        reference.enableIPCP = true;
        client.request("hello", {{"client", "test"}, {"clock_hz", 1000}, {"auth_key", std::string(64, 'a')}}); client.exchange(host, time, 1);
        open();
    }
    void open() {
        const auto count = client.messages.size(); client.request("open", {{"target", "local-service"}, {"subscriber", "0001"}});
        client.exchange(host, time, count + 1); call = client.messages.back()["body"]["call"].get<std::string>();
    }
    void batch(const Bytes &input, unsigned frame) {
        for (auto byte : input) reference.feed(byte, frame); reference.tick(frame);
        const auto expected = reference.out; reference.out.clear();
        if (!input.empty()) {
            std::string encoded; check(encodeBase64(input, encoded), "base64"); const auto count = client.messages.size();
            client.request("data", {{"call", call}, {"offset", std::to_string(offset)}, {"bytes", input.size()}, {"payload_b64", encoded}});
            offset += input.size(); client.exchange(host, time, count + 1);
            check(client.messages.back()["type"] == "data_ack", "receipt ACK");
        }
        const auto start = client.messages.size(); const auto request = client.next;
        client.request("advance", {{"call", call}, {"tick", std::to_string(uint64_t{frame} * 1000)}});
        // Diagnostic fixtures emit less than a single 4096-byte data chunk.
        const auto count = start + 1 + (input.empty() ? 0 : 1) + (expected.empty() ? 0 : 1);
        client.exchange(host, time, count); Bytes actual;
        for (size_t i = start; i < count; ++i) {
            const auto &m = client.messages[i];
            if (m["type"] != "data") continue;
            check(m["body"]["offset"] == std::to_string(output), "reply offset");
            std::array<uint8_t, 4096> bytes{}; size_t n = 0;
            check(decodeBase64(m["body"]["payload_b64"].get_ref<const std::string &>(), bytes, n), "reply decode");
            actual.insert(actual.end(), bytes.begin(), bytes.begin() + n); output += n;
        }
        check(actual == expected && client.messages.back()["type"] == "advance_ok" &&
            client.messages.back()["reply_to"] == std::to_string(request), "diagnostic host byte parity");
        if (output) client.request("data_ack", {{"call", call}, {"next_offset", std::to_string(output)}, {"limit", std::to_string(output + 65536)}});
    }
};
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        auto test = std::make_unique<Test>(); const std::string_view name = argv[1];
        if (name == "host_ppp_negotiation") {
            test->batch(packet(0xc021, 1, 1), 0); test->batch(packet(0xc021, 2, 0x51), 1);
            test->batch(packet(0x8021, 1, 2, {3, 6, 0, 0, 0, 0}), 2);
            test->batch(packet(0x8021, 1, 3, {3, 6, 10, 0, 0, 2}), 3);
            test->batch(packet(0x8021, 2, 0x61, {3, 6, 10, 0, 0, 1}), 4);
            check(test->reference.ipOpen(), "IPCP reference open");
        } else if (name == "host_ppp_reset") {
            test->batch(Bytes{0x7e, 0x7d}, 0);
            auto count = test->client.messages.size(); const auto previous = test->call;
            test->client.request("close", {{"call", previous}, {"reason", "reset"}}); test->client.exchange(test->host, test->time, count + 1);
            test->open(); check(previous != test->call, "fresh call"); test->reference.resetSession(); test->offset = test->output = 0;
            test->batch(packet(0xc021, 1, 1), 0); test->batch({}, 180);
        } else throw std::runtime_error("unknown test");
        test->host.stop(); check(test->host.clientCount() == 0, "all peers disposed");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
