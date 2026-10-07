#include <xband/service_control.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
constexpr std::string_view token_value = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
class Probe : public ServiceEndpoint {
public:
    unsigned resets = 0; bool throws = false;
    bool transmit(uint8_t, unsigned) override { throw std::runtime_error("unexpected transmit"); }
    void tick(unsigned) override { throw std::runtime_error("unexpected tick"); }
    bool peek(uint8_t &) const override { return false; }
    void consume() override { throw std::runtime_error("unexpected consume"); }
    size_t pending() const override { return 0; }
    void reset() override { ++resets; if (throws) throw std::runtime_error("internal details"); }
};
uint64_t activate(ConnectionLifecycle &c) {
    const auto g = c.begin(0); c.hello(g, "saturn-1", 0);
    check(c.authenticationCompleted(g, true, token_value, 0), "host approval"); return g;
}
void request(ServerInbox &inbox, std::string type, uint64_t id, Json body) {
    const auto s = Json{{"v", 2}, {"type", type}, {"endpoint", "saturn-1"}, {"session", token_value},
        {"id", std::to_string(id)}, {"reply_to", nullptr}, {"body", body}}.dump();
    std::vector<uint8_t> bytes(s.size() + 4);
    encodeFrame(std::span(reinterpret_cast<const uint8_t *>(s.data()), s.size()), bytes);
    check(inbox.feed(bytes, id).status == ServerInbox::Status::ready, "request ready");
}
Json response(FrameQueue &q) {
    MessageInbox inbox;
    while (!inbox.peek()) {
        const auto r = inbox.feed(q.peek()); check(r.consumed != 0, "response progress"); q.consume(r.consumed);
    }
    return *inbox.peek();
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view test = argv[1]; ConnectionLifecycle c; const auto g = activate(c);
        ServerInbox inbox(c, g); FrameQueue q; Probe service;
        SessionOutbox outbox(q, c, g, "saturn-1", std::string(token_value));
        ServiceControl control(inbox, c, g, outbox, service);
        const Json open{{"target", "local-service"}, {"subscriber", "00123"}};
        if (test == "service_replies") {
            request(inbox, "ping", 2, Json::object()); check(control.process() == ServiceControl::Outcome::handled, "ping handled");
            auto r = response(q); check(r["type"] == "pong" && r["id"] == "2" && r["reply_to"] == "2" && service.resets == 0, "pong passive");
            request(inbox, "open", 3, open); check(control.process(token_value) == ServiceControl::Outcome::handled, "open handled");
            r = response(q); check(r["type"] == "open_ok" && r["id"] == "3" && service.resets == 1 && control.subscriber() == "00123", "open reset once");
            check(control.process() == ServiceControl::Outcome::idle && service.resets == 1, "no replay");
            request(inbox, "open", 4, open); check(control.process() == ServiceControl::Outcome::handled, "busy handled");
            check(response(q)["body"]["code"] == "BUSY" && service.resets == 1, "busy leaves service");
            request(inbox, "close", 5, {{"call", "ffffffffffffffffffffffffffffffff"}, {"reason", "hangup"}});
            check(control.process() == ServiceControl::Outcome::handled && response(q)["body"]["code"] == "STALE_CALL" && service.resets == 1, "old close cannot reset");
            request(inbox, "close", 6, {{"call", token_value}, {"reason", "hangup"}});
            check(control.process() == ServiceControl::Outcome::handled && response(q)["type"] == "close_ok" && service.resets == 2 && control.call().empty(), "close cleanup");
        } else if (test == "service_backpressure_reply") {
            const std::array<uint8_t, 1> byte{1};
            for (size_t i = 0; i < FrameQueue::message_capacity; ++i) q.enqueue(byte);
            request(inbox, "open", 2, open);
            check(control.process(token_value) == ServiceControl::Outcome::blocked && service.resets == 0 && control.call().empty() && inbox.peek(), "full queue no service mutation");
            q.reset(); check(control.process(token_value) == ServiceControl::Outcome::handled, "retry after space");
            check(response(q)["id"] == "2" && service.resets == 1, "no skipped ID or double reset");
        } else if (test == "service_failure_reply") {
            service.throws = true; request(inbox, "open", 2, open);
            check(control.process(token_value) == ServiceControl::Outcome::failed && q.pendingBytes() == 0, "success reply discarded on reset failure");
            check(c.state() == ConnectionLifecycle::State::closed && control.call().empty(), "session closed");
            check(control.process(token_value) == ServiceControl::Outcome::failed && service.resets == 1, "no repeated throwing reset");
        } else if (test == "output_mixed") {
            request(inbox, "open", 2, open); check(control.process(token_value) == ServiceControl::Outcome::handled, "open reply");
            SendWindow sender;
            const Json data{{"call", token_value}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}};
            check(outbox.data(sender, control.call(), data) == Admission::queued, "data shares sequencer");
            request(inbox, "ping", 3, Json::object()); check(control.process() == ServiceControl::Outcome::handled, "pong after data");
            auto a = response(q), b = response(q), d = response(q);
            check(a["type"] == "open_ok" && a["id"] == "2" && b["type"] == "data" && b["id"] == "3" &&
                d["type"] == "pong" && d["id"] == "4" && outbox.nextId() == 5, "shared ordered IDs");
            check(sender.nextOffset() == 3, "data accounted once");
        } else if (test == "output_rejections") {
            SendWindow sender; const Json data{{"call", token_value}, {"offset", "0"}, {"bytes", 1}, {"payload_b64", "QQ=="}};
            check(outbox.control("data", data) == Admission::invalid && outbox.nextId() == 2, "cannot bypass credit via control");
            check(outbox.control("ping", {{"extra", 1}}) == Admission::invalid && outbox.nextId() == 2, "bad schema no ID loss");
            const std::array<uint8_t, 1> byte{1};
            for (size_t i = 0; i < FrameQueue::message_capacity; ++i) q.enqueue(byte);
            check(outbox.data(sender, token_value, data) == Admission::queue_full && outbox.nextId() == 2 && sender.nextOffset() == 0, "queue full rollback");
            q.reset();
            for (unsigned i = 0; i < 16; ++i) sender.commit(4096);
            auto blocked = data; blocked["offset"] = "65536";
            check(outbox.data(sender, token_value, blocked) == Admission::credit_full && outbox.nextId() == 2 && q.pendingBytes() == 0, "credit full rollback");
            check(outbox.control("ping", Json::object()) == Admission::queued && response(q)["id"] == "2", "control independent of data credit");
            activate(c);
            check(outbox.control("ping", Json::object()) == Admission::inactive && q.pendingBytes() == 0, "old generation blocked");
        } else if (test == "output_exhaustion") {
            SessionOutbox last(q, c, g, "saturn-1", std::string(token_value), UINT64_MAX);
            check(last.control("ping", Json::object()) == Admission::queued && last.exhausted(), "last ID queued");
            check(last.control("ping", Json::object()) == Admission::inactive && q.pendingMessages() == 1, "no wrap or duplicate");
            check(response(q)["id"] == "18446744073709551615", "exact ID as string");
            last.discard(); check(last.control("ping", Json::object()) == Admission::inactive, "discard disables writer");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
