#include <xband/server_inbox.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
constexpr std::string_view session = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
uint64_t activate(ConnectionLifecycle &c) {
    auto g = c.begin(0); check(c.hello(g, "saturn-1", 0), "hello");
    check(c.authenticationCompleted(g, true, session, 0), "host approval"); return g;
}
Json message(std::string type = "ping", std::string id = "2") {
    return {{"v", 2}, {"type", type}, {"endpoint", "saturn-1"}, {"session", session},
        {"id", id}, {"reply_to", nullptr}, {"body", Json::object()}};
}
Bytes wire(const Json &j) {
    auto s = j.dump(); Bytes out(s.size() + 4);
    check(encodeFrame(std::span(reinterpret_cast<const uint8_t *>(s.data()), s.size()), out).status == EncodeStatus::ok, "encode"); return out;
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2; const std::string_view test = argv[1];
        ConnectionLifecycle c; auto g = activate(c);
        if (test == "routing") {
            ServerInbox inbox(c, g);
            auto a = wire(message()), b = wire(message("ping", "3")); auto both = a; both.insert(both.end(), b.begin(), b.end());
            auto r = inbox.feed(both, 1);
            check(r.consumed == a.size() && inbox.route() == ServerInbox::Route::ping, "one routed message");
            check(inbox.peek() && inbox.peek() && inbox.feed(b, 2).consumed == 0, "blocked consumer does not recheck ID");
            check(inbox.consume() && !inbox.consume(), "consume exactly once");
            check(inbox.feed(std::span(both).subspan(r.consumed), 3).status == ServerInbox::Status::ready, "next ID valid");
            inbox.consume();
            check(inbox.feed(b, 4).status == ServerInbox::Status::failed && !inbox.peek(), "duplicate ID refused");
        } else if (test == "routing_identity") {
            for (auto field : {"endpoint", "session"}) {
                g = activate(c); ServerInbox inbox(c, g); auto m = message();
                m[field] = std::string(field) == "endpoint" ? "saturn-2" : "ffffffffffffffffffffffffffffffff";
                check(inbox.feed(wire(m), 1).status == ServerInbox::Status::failed && !inbox.peek(), "foreign identity refused");
            }
            g = activate(c); ServerInbox inbox(c, g);
            auto m = message("open_ok"); m["reply_to"] = "1"; m["body"] = {{"call", session}};
            check(inbox.feed(wire(m), 1).status == ServerInbox::Status::failed, "server-only reply refused");
            check(c.state() == ConnectionLifecycle::State::closed, "bad direction closes connection");
        } else if (test == "routing_generation") {
            ServerInbox old(c, g); check(old.feed(wire(message()), 1).status == ServerInbox::Status::ready, "old pending");
            auto fresh = activate(c);
            check(!old.peek() && !old.consume(), "old pending hidden after reconnect");
            check(old.feed({}, 90000).status == ServerInbox::Status::failed && c.state() == ConnectionLifecycle::State::active, "old callback cannot close new connection");
            ServerInbox current(c, fresh);
            check(current.feed(wire(message()), 1).status == ServerInbox::Status::ready, "new connection works");
            check(current.feed({}, 10001).status == ServerInbox::Status::failed && !current.peek(), "pending does not defeat idle timeout");
        } else if (test == "routing_categories") {
            const std::array types{"open", "data", "data_ack", "advance", "snapshot", "ping", "pong", "error"};
            const std::array routes{ServerInbox::Route::service, ServerInbox::Route::data, ServerInbox::Route::acknowledgement,
                ServerInbox::Route::clock, ServerInbox::Route::snapshot, ServerInbox::Route::ping, ServerInbox::Route::pong, ServerInbox::Route::error};
            for (size_t i = 0; i < types.size(); ++i) {
                g = activate(c); ServerInbox inbox(c, g); auto m = message(types[i]); auto &body = m["body"];
                if (i == 0) body = {{"target", "local-service"}, {"subscriber", "00123"}};
                if (i == 1) body = {{"call", session}, {"offset", "0"}, {"bytes", 1}, {"payload_b64", "QQ=="}};
                if (i == 2) body = {{"call", session}, {"next_offset", "0"}, {"limit", "65536"}};
                if (i == 3) body = {{"call", session}, {"tick", "0"}};
                if (i == 4) body = {{"call", nullptr}, {"subscriber", nullptr}, {"state", "idle"}, {"sent_bytes", "0"}, {"received_bytes", "0"},
                    {"card", {{"inserted", false}, {"read_state", "absent"}, {"number", nullptr}, {"remaining_units", nullptr}, {"nominal_units", nullptr}}}};
                if (i == 6) m["reply_to"] = "1";
                if (i == 7) body = {{"code", "INTERNAL_ERROR"}, {"message", "test"}, {"scope", "connection"}, {"call", nullptr}};
                check(inbox.feed(wire(m), 1).status == ServerInbox::Status::ready && inbox.route() == routes[i], "route classification");
            }
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
