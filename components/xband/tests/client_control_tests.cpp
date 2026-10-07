#include <xband/client_control.hpp>
#include <xband/server_session.hpp>
#include <iostream>
#include <deque>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
constexpr auto session_id = "0123456789abcdef0123456789abcdef";
constexpr auto call_id = "abcdef0123456789abcdef0123456789";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Empty : ServiceEndpoint {
    bool transmit(uint8_t, unsigned) override { throw std::runtime_error("unexpected guest byte"); }
    void tick(unsigned) override { throw std::runtime_error("unexpected guest tick"); }
    bool peek(uint8_t &) const override { return false; }
    void consume() override { throw std::runtime_error("unexpected consume"); }
    size_t pending() const override { return 0; }
    void reset() override {}
};
Json card() { return {{"inserted", true}, {"read_state", "readable"}, {"number", "960500132270"}, {"remaining_units", 7}, {"nominal_units", 10}}; }
struct Echo : ServiceEndpoint {
    std::deque<uint8_t> bytes;
    unsigned frame = 0; size_t transmitted = 0, ticks = 0;
    bool transmit(uint8_t byte, unsigned f) override { bytes.push_back(byte); frame = f; ++transmitted; return true; }
    void tick(unsigned f) override { frame = f; ++ticks; }
    bool peek(uint8_t &byte) const override { if (bytes.empty()) return false; byte = bytes.front(); return true; }
    void consume() override { bytes.pop_front(); }
    size_t pending() const override { return bytes.size(); }
    void reset() override { bytes.clear(); transmitted = ticks = 0; frame = 0; }
};
struct Pair {
    Echo *echo = nullptr;
    EndpointRegistry registry{{{"modem1", std::string(64, 'a')}}};
    ServerSession server{registry, 0, session_id, FrameClock(1, 1000), [this](uint64_t hz) {
        check(hz == 1000, "clock passed unchanged"); auto service = std::make_unique<Echo>(); echo = service.get(); return service; }};
    ClientControl client{"modem1", std::string(64, 'a'), 1000, 0};
    void exchange(uint64_t now) {
        for (unsigned i = 0; i < 10000; ++i) {
            check(server.poll(now, call_id) && client.poll(now), "pair alive");
            bool moved = false;
            if (!client.output().empty()) {
                auto input = client.output().first(std::min<size_t>(7, client.output().size()));
                auto r = server.feed(input, now, call_id);
                check(r.status != MessageInbox::Status::failed && client.sent(r.consumed), "client to server"); moved |= r.consumed != 0;
            }
            if (!server.output().empty()) {
                // Account socket write before delivering copied bytes to client.
                auto bytes = server.output().first(std::min<size_t>(11, server.output().size()));
                std::vector<uint8_t> copied(bytes.begin(), bytes.end());
                check(server.sent(copied.size(), now), "server sent");
                size_t cursor = 0;
                while (cursor < copied.size()) {
                    auto r = client.feed(std::span(copied).subspan(cursor), now);
                    check(r.status != MessageInbox::Status::failed && r.consumed != 0, "server to client"); cursor += r.consumed;
                }
                moved = true;
            }
            // Server may make budgeted service progress without producing wire
            // bytes in this iteration. An outstanding advance is not quiescent.
            if (!moved && !client.advancing()) return;
        }
        throw std::runtime_error("unbounded exchange");
    }
};
void inject(ClientControl &client, std::string type, Json body, Json reply, uint64_t now, std::string session = session_id, std::string message_id = "2") {
    const auto text = Json{{"v",2},{"type",type},{"endpoint","modem1"},{"session",session},{"id",message_id},{"reply_to",reply},{"body",body}}.dump();
    std::vector<uint8_t> encoded(text.size()+4);
    encodeFrame(std::span(reinterpret_cast<const uint8_t *>(text.data()),text.size()),encoded);
    client.feed(encoded, now);
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; auto p = std::make_unique<Pair>();
        if (name == "client_control_roundtrip") {
            p->exchange(1); check(p->client.state() == ClientControl::State::idle, "hello completed");
            check(p->client.open("003336666666", 2) == Admission::queued, "open queued");
            check(p->client.open("003336666665", 2) == Admission::inactive, "second open refused");
            p->exchange(2); check(p->client.call() == call_id, "server issued call");
            const auto original = card();
            check(p->client.snapshot(original, 12, 13, 3) == Admission::queued, "snapshot queued"); p->exchange(3);
            check(p->server.snapshot() && (*p->server.snapshot())["card"] == original && card() == original, "snapshot card unchanged");
            check((*p->server.snapshot())["subscriber"] == "003336666666", "subscriber bound to call");
            check(p->client.close(4) == Admission::queued, "close queued"); p->exchange(4);
            check(p->client.state() == ClientControl::State::idle && p->client.call().empty() && !p->server.snapshot(), "hangup cleared both sides");
        } else if (name == "client_data_roundtrip") {
            p->exchange(1); p->client.open("0001", 2); p->exchange(2);
            const std::array<uint8_t, 5> data{0, 255, 1, 128, 42};
            check(p->client.transmit(data, 3) == Admission::queued, "data admitted"); p->exchange(3);
            check(p->echo->transmitted == 0 && p->client.received().empty(), "data staged until guest tick");
            check(p->client.advance(2500, 4) == Admission::queued, "tick admitted");
            check(p->client.transmit(data, 4) == Admission::inactive && p->client.advance(3000, 4) == Admission::inactive &&
                p->client.close(4) == Admission::inactive, "one outstanding advance");
            p->exchange(4);
            check(!p->client.advancing() && p->client.completedTick() == 2500 && p->echo->frame == 2, "guest time not host milliseconds");
            check(std::equal(data.begin(), data.end(), p->client.received().begin(), p->client.received().end()), "binary exact echo");
            check(p->client.consumeReceived(2, 5), "partial guest consumption"); p->exchange(5);
            check(p->client.received().size() == 3 && p->echo->transmitted == 5 && p->echo->ticks == 1, "no duplicate effects");
            check(p->client.advance(2499, 6) == Admission::invalid, "guest clock reversal rejected locally");
        } else if (name == "client_data_credit") {
            p->exchange(1); p->client.open("0001", 2); p->exchange(2);
            std::array<uint8_t, 4096> data{};
            for (unsigned i = 0; i < 16; ++i) {
                check(p->client.transmit(data, 3) == Admission::queued, "fill send window"); p->exchange(3);
            }
            check(p->client.transmit(data, 3) == Admission::credit_full, "bounded remote capacity");
            check(p->client.advance(1000, 4) == Admission::queued, "process full batch"); p->exchange(4);
            check(!p->client.advancing() && p->client.received().size() == 65536, "full receive window");
            check(p->client.consumeReceived(4096, 5), "release receive credit"); p->exchange(5);
            check(p->client.transmit(data, 6) == Admission::queued && p->client.advance(2000, 6) == Admission::queued, "reuse consumed capacity");
            p->exchange(6); check(!p->client.advancing() && p->echo->transmitted == 69632, "wraparound batch completed");
        } else if (name == "client_data_reset") {
            p->exchange(1); p->client.open("0001", 2); p->exchange(2);
            const std::array<uint8_t, 1> data{42}; p->client.transmit(data, 3); p->client.advance(1000, 3); p->exchange(3);
            check(p->client.close(4) == Admission::queued, "hangup with unread data"); p->exchange(4);
            check(p->client.received().empty() && p->client.completedTick() == 0, "call state cleared");
            p->client.open("0001", 5); p->exchange(5); p->client.transmit(data, 6); p->client.advance(0, 6); p->exchange(6);
            check(p->client.received().size() == 1 && p->echo->transmitted == 1, "new call offsets reset");
        } else if (name == "client_data_reject") {
            for (unsigned variant = 0; variant < 4; ++variant) {
                auto q = std::make_unique<Pair>(); q->exchange(1); q->client.open("0001", 2); q->exchange(2);
                q->client.advance(1000, 3);
                if (variant == 0) inject(q->client, "data", {{"call", call_id}, {"offset", "1"}, {"bytes", 1}, {"payload_b64", "Kg=="}}, nullptr, 4, session_id, "3");
                if (variant == 1) inject(q->client, "data_ack", {{"call", call_id}, {"next_offset", "1"}, {"limit", "65536"}}, nullptr, 4, session_id, "3");
                if (variant == 2) inject(q->client, "advance_ok", {{"call", call_id}, {"tick", "999"}}, "3", 4, session_id, "3");
                if (variant == 3) inject(q->client, "data", {{"call", session_id}, {"offset", "0"}, {"bytes", 1}, {"payload_b64", "Kg=="}}, nullptr, 4, session_id, "3");
                check(q->client.state() == ClientControl::State::stopped && q->client.received().empty(), "invalid transfer fails closed");
            }
        } else if (name == "client_data_timeout") {
            p->exchange(1); p->client.open("0001", 2); p->exchange(2);
            p->client.advance(1000, 3);
            check(!p->client.poll(5003) && !p->client.advancing() && p->client.received().empty(), "advance deadline clears transfer");
        } else if (name == "client_control_heartbeat") {
            p->exchange(1); p->exchange(2001); p->exchange(4001); p->exchange(6001);
            check(p->client.state() == ClientControl::State::idle, "idle server ping answered repeatedly");
        } else if (name == "client_control_backpressure") {
            p->exchange(1);
            unsigned queued = 0;
            while (p->client.snapshot(card(), 0, 0, 2) == Admission::queued) {
                check(++queued <= 128, "bounded queue");
            }
            check(queued > 0 && p->client.state() == ClientControl::State::idle, "queue full does not disconnect");
            p->exchange(2001);
            check(p->client.state() == ClientControl::State::idle && p->client.output().empty(), "pending pong resumes without duplicate id");
        } else if (name == "client_control_timeout") {
            check(!p->client.poll(5000) && p->client.output().empty(), "hello deadline closes and clears");
            auto second = std::make_unique<Pair>(); second->exchange(1); second->client.open("0001", 2);
            check(!second->client.poll(5002) && second->client.output().empty(), "open deadline closes");
        } else if (name == "client_control_reject") {
            p->exchange(1); p->client.open("0001", 2);
            inject(p->client, "open_ok", {{"call", call_id}}, "999", 3);
            check(p->client.state() == ClientControl::State::stopped && p->client.call().empty(), "bad correlation no carrier");
            auto second = std::make_unique<Pair>(); second->exchange(1); second->client.open("0001", 2);
            inject(second->client, "open_ok", {{"call", call_id}}, "2", 3, "ffffffffffffffffffffffffffffffff");
            check(second->client.state() == ClientControl::State::stopped, "stale session rejected");
        } else if (name == "client_control_local_validation") {
            p->exchange(1); check(p->client.open("invalid", 2) == Admission::invalid, "phone validation");
            check(p->client.snapshot(card(), 1, 0, 2) == Admission::invalid, "idle counters cannot imply data transfer");
            auto bad = card(); bad["remaining_units"] = 101;
            check(p->client.snapshot(bad, 0, 0, 2) == Admission::invalid, "invalid card refused");
            check(p->client.output().empty(), "invalid requests do not consume output ids");
            check(p->client.open("0001", 3) == Admission::queued, "still usable"); p->exchange(3);
            p->client.disconnect(); check(p->client.output().empty() && p->client.call().empty(), "EOF clears state");
        } else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
