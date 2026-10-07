#include <xband/service_connection.hpp>
#include <iostream>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
constexpr std::string_view id = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Empty : ServiceEndpoint {
    bool transmit(uint8_t, unsigned) override { throw std::runtime_error("unexpected guest input"); }
    void tick(unsigned) override { throw std::runtime_error("unexpected guest tick"); }
    bool peek(uint8_t &) const override { return false; }
    void consume() override {}
    size_t pending() const override { return 0; }
    void reset() override {}
};
uint64_t activate(ConnectionLifecycle &c) { auto g = c.begin(0); c.hello(g, "test", 0); check(c.authenticationCompleted(g, true, id, 0), "activate"); return g; }
struct Fixture {
    ConnectionLifecycle lifecycle; uint64_t generation = activate(lifecycle), next = 2;
    ServiceConnection service{lifecycle, generation, "test", std::string(id), std::make_unique<Empty>(), FrameClock(1, 1000)};
    void send(std::string type, Json body, uint64_t now, Json reply = nullptr) {
        const auto text = Json{{"v", 2}, {"type", type}, {"endpoint", "test"}, {"session", id}, {"id", std::to_string(next++)},
            {"reply_to", reply}, {"body", body}}.dump();
        std::vector<uint8_t> bytes(text.size() + 4); encodeFrame(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), bytes);
        check(service.feed(bytes, now).status == MessageInbox::Status::ready, "input parsed");
    }
    std::vector<Json> drain() {
        MessageInbox decoded; std::vector<Json> result;
        while (!service.output().empty()) {
            auto step = decoded.feed(service.output()); check(step.consumed != 0 && service.sent(step.consumed), "drain");
            if (decoded.peek()) { result.push_back(*decoded.peek()); decoded.consume(); }
        }
        return result;
    }
};
Json snapshot() { return {{"call", nullptr}, {"subscriber", nullptr}, {"state", "idle"}, {"sent_bytes", "0"}, {"received_bytes", "0"},
    {"card", {{"inserted", false}, {"read_state", "absent"}, {"number", nullptr}, {"remaining_units", nullptr}, {"nominal_units", nullptr}}}}; }
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2; auto f = std::make_unique<Fixture>(); const std::string_view name = argv[1];
        if (name == "status_heartbeat") {
            f->service.poll(1999); check(f->drain().empty(), "not early"); f->service.poll(2000);
            auto ping = f->drain(); check(ping.size() == 1 && ping[0]["type"] == "ping", "one ping");
            f->service.poll(4000); check(f->drain().empty(), "no duplicate outstanding ping");
            f->send("pong", Json::object(), 4001, ping[0]["id"]); check(f->service.poll(4001) == ServiceConnection::Outcome::handled, "correlated pong");
            f->service.poll(6000); check(f->drain().empty(), "traffic resets idle schedule");
            f->service.poll(6001); check(f->drain().size() == 1, "next idle ping");
        } else if (name == "status_deadline") {
            f->service.poll(2000); f->drain(); f->send("snapshot", snapshot(), 6999); f->service.poll(6999);
            check(f->service.snapshot() != nullptr, "unrelated status accepted");
            check(f->service.poll(7000) == ServiceConnection::Outcome::failed && !f->service.snapshot(), "status cannot extend ping deadline");
        } else if (name == "status_wrong_pong") {
            f->service.poll(2000); f->drain(); f->send("pong", Json::object(), 2001, "999");
            check(f->service.poll(2001) == ServiceConnection::Outcome::failed, "wrong correlation closes");
        } else if (name == "status_snapshot") {
            f->send("snapshot", snapshot(), 1); f->service.poll(1); check(f->service.snapshot() && f->drain().empty(), "status retained without reply");
            f->send("open", {{"target", "local-service"}, {"subscriber", "0001"}}, 2); f->service.poll(2, id); f->drain();
            check(!f->service.snapshot(), "open clears stale idle status");
            auto body = snapshot(); body["call"] = id; body["subscriber"] = "0001"; body["state"] = "service";
            body["card"] = {{"inserted", true}, {"read_state", "readable"}, {"number", "960500000001"}, {"remaining_units", 7}, {"nominal_units", 10}};
            f->send("snapshot", body, 3); f->service.poll(3);
            check(f->service.snapshot() && (*f->service.snapshot())["card"]["remaining_units"] == 7, "peer card view only");
            body["call"] = "ffffffffffffffffffffffffffffffff"; f->send("snapshot", body, 4);
            check(f->service.poll(4) == ServiceConnection::Outcome::failed && !f->service.snapshot(), "stale call rejected, status cleared");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
