#include "host_fixture.hpp"
#include "../tools/diagnostic_endpoint.hpp"
#include <xband/windows_tcp_client.hpp>
#include <xband/remote_service_batch.hpp>

using Bytes = std::vector<uint8_t>;
Bytes packet(uint16_t protocol, uint8_t code, uint8_t id, Bytes options = {}) {
    const auto size = options.size() + 4;
    Bytes bytes{0xff, 3, static_cast<uint8_t>(protocol >> 8), static_cast<uint8_t>(protocol), code, id,
        static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
    bytes.insert(bytes.end(), options.begin(), options.end());
    return LocalPPPProbe::encode(bytes);
}
struct Test {
    // Synthetic 60-frame/s fixture, NOT an assertion about Saturn video timing.
    Host host{configuration(), FrameClock(60, 1000000000), [](uint64_t hz) -> std::unique_ptr<ServiceEndpoint> {
        check(hz == 1000000000, "nanosecond clock"); return std::make_unique<OwnedDiagnosticEndpoint>();
    }, Host::PortMode::loopback_ephemeral_test};
    xband::windows::TcpClient client{{"127.0.0.1", host.port(), false, "test", std::string(64, 'a'), 1000000000}, 0};
    LocalPPPProbe reference;
    uint64_t time = 0;
    Test() {
        reference.enableIPCP = true;
        pump([&] { return client.control() && client.control()->state() == ClientControl::State::idle; });
        open();
    }
    template<class Done> void pump(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            check(host.step(++time) && client.step(time), "live TCP pump");
            // step failure destroys ClientControl: never access a borrowed batch after it.
            if (done()) return;
            Sleep(1);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("PPP client timeout");
    }
    void open() {
        check(client.control()->open("0001", time) == Admission::queued, "open");
        pump([&] { return client.control()->state() == ClientControl::State::service; });
    }
    Bytes batch(const Bytes &input, uint64_t tick, unsigned expected_frame) {
        for (auto byte : input) reference.feed(byte, expected_frame);
        reference.tick(expected_frame);
        const auto expected = reference.out; reference.out.clear();
        RemoteServiceBatch transfer(*client.control());
        check(transfer.begin(input, tick), "begin PPP batch");
        Bytes actual; bool blocked = false;
        pump([&] {
            const auto result = transfer.resume(time, [&](uint8_t byte) {
                if (!blocked) { blocked = true; return false; }
                actual.push_back(byte); return true;
            }, 3);
            check(result != RemoteServiceBatch::Step::failed, "PPP batch alive");
            return result == RemoteServiceBatch::Step::done;
        });
        check(actual == expected, "PPP byte-for-byte parity");
        check(expected.empty() || blocked, "sink backpressure exercised");
        check(transfer.resume(time, [](uint8_t) -> bool { throw std::runtime_error("duplicate"); }) ==
            RemoteServiceBatch::Step::idle, "no repeated delivery");
        return actual;
    }
    static uint64_t at(unsigned frame) { return (uint64_t{frame} * 1000000000 + 59) / 60; }
};
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        auto test = std::make_unique<Test>(); const std::string_view name = argv[1];
        if (name == "client_ppp_negotiation") {
            test->batch(packet(0xc021, 1, 1), 0, 0);
            test->batch(packet(0xc021, 2, 0x51), Test::at(1), 1);
            test->batch(packet(0x8021, 1, 2, {3, 6, 0, 0, 0, 0}), Test::at(2), 2);
            test->batch(packet(0x8021, 1, 3, {3, 6, 10, 0, 0, 2}), Test::at(3), 3);
            test->batch(packet(0x8021, 2, 0x61, {3, 6, 10, 0, 0, 1}), Test::at(4), 4);
            check(test->reference.ipOpen(), "IPCP negotiated");
        } else if (name == "client_ppp_fragments") {
            const auto bytes = packet(0xc021, 1, 1);
            for (auto byte : bytes) test->batch(Bytes{byte}, 0, 0);
            check(test->batch({}, Test::at(180) - 1, 179).empty(), "no early retry");
            check(!test->batch({}, Test::at(180), 180).empty(), "retry at guest boundary");
        } else if (name == "client_ppp_reset") {
            test->batch({0x7e, 0x7d}, Test::at(20), 20);
            const auto previous = test->client.control()->call();
            check(test->client.control()->close(test->time) == Admission::queued, "close incomplete frame");
            test->pump([&] { return test->client.control()->state() == ClientControl::State::idle; });
            test->open(); check(test->client.control()->call() != previous, "new call identity");
            test->reference.resetSession();
            test->batch(packet(0xc021, 1, 1), 0, 0);
            test->batch({}, Test::at(180), 180);
        } else throw std::runtime_error("unknown test");
        test->client.stop(); test->host.stop();
        check(test->host.clientCount() == 0, "sockets disposed");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
