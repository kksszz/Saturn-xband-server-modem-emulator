#include <xband/server_handshake.hpp>
#include <xband/server_inbox.hpp>
#include <iostream>
#include <memory>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
constexpr std::string_view session = "0123456789abcdef0123456789abcdef";
const std::string key(64, 'a');
void check(bool value, const char *why) { if (!value) throw std::runtime_error(why); }
Json hello() { return {{"v", 2}, {"type", "hello"}, {"endpoint", "modem1"}, {"session", ""}, {"id", "1"},
    {"reply_to", nullptr}, {"body", {{"client", "test"}, {"clock_hz", 1000000}, {"auth_key", key}}}}; }
std::vector<uint8_t> frame(const Json &message) {
    const auto text = message.dump(); std::vector<uint8_t> bytes(text.size() + 4);
    check(encodeFrame(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), bytes).written == bytes.size(), "frame"); return bytes;
}
struct Peer {
    ConnectionLifecycle lifecycle; uint64_t generation = lifecycle.begin(0);
    ServerHandshake handshake;
    explicit Peer(EndpointRegistry &registry) : handshake(lifecycle, generation, registry) {}
    void feed(Json message = hello(), uint64_t now = 0) {
        auto bytes = frame(message); handshake.feed(bytes, now, session);
    }
    Json finish(uint64_t now = 0) {
        MessageInbox decoded;
        while (!handshake.output().empty()) {
            auto bytes = handshake.output().first(1);
            check(decoded.feed(bytes).consumed == 1 && handshake.sent(1, now), "partial reply send");
        }
        check(decoded.peek() != nullptr, "hello reply decoded"); return *decoded.peek();
    }
};
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; EndpointRegistry registry({{"modem1", key}, {"modem2", key}});
        auto peer = std::make_unique<Peer>(registry);
        if (name == "handshake_accept") {
            auto wire = frame(hello());
            for (const auto byte : wire) check(peer->handshake.feed(std::span(&byte, 1), 1, session).consumed == 1, "fragmented hello");
            check(peer->handshake.state() == ServerHandshake::State::replying && peer->lifecycle.state() == ConnectionLifecycle::State::verifying && peer->handshake.session().empty(), "no early activation");
            check(peer->handshake.sent(0, 2) && peer->handshake.state() == ServerHandshake::State::replying, "would-block preserves state");
            const auto reply = peer->finish(3);
            check(reply["type"] == "hello_ok" && reply["id"] == "1" && reply["reply_to"] == "1" && reply["body"]["new_session"].get<std::string>() == session, "hello reply");
            check(peer->handshake.state() == ServerHandshake::State::accepted && peer->handshake.clockHz() == 1000000, "accepted metadata");
            ServerInbox service(peer->lifecycle, peer->generation);
            auto request = frame(Json{{"v", 2}, {"type", "open"}, {"endpoint", "modem1"}, {"session", session}, {"id", "2"},
                {"reply_to", nullptr}, {"body", {{"target", "local-service"}, {"subscriber", "0001"}}}});
            check(service.feed(request, 4).status == ServerInbox::Status::ready, "service sequence starts at two");
        } else if (name == "handshake_reject") {
            for (int variant = 0; variant < 5; ++variant) {
                auto bad = std::make_unique<Peer>(registry); auto m = hello();
                if (variant == 0) m["body"]["auth_key"] = std::string(64, 'b');
                if (variant == 1) m["endpoint"] = "unknown";
                if (variant == 2) m["id"] = "2";
                if (variant == 3) m["v"] = 1;
                if (variant == 4) { m["type"] = "ping"; m["session"] = session; m["body"] = Json::object(); }
                bad->feed(m); check(bad->handshake.state() == ServerHandshake::State::failed && bad->handshake.output().empty() && bad->lifecycle.state() == ConnectionLifecycle::State::closed, "rejected without reply/service");
            }
            peer->feed(); peer->finish(); check(peer->handshake.state() == ServerHandshake::State::accepted, "rejections do not reserve identity");
        } else if (name == "handshake_reservation") {
            peer->feed(); auto duplicate = std::make_unique<Peer>(registry); duplicate->feed();
            check(duplicate->handshake.failure() == ServerHandshake::Failure::busy, "pending reply reserves identity");
            peer->finish(); duplicate.reset();
            auto still_busy = std::make_unique<Peer>(registry); still_busy->feed();
            check(still_busy->handshake.failure() == ServerHandshake::Failure::busy, "loser cleanup cannot release winner");
            peer->handshake.disconnect(); auto replacement = std::make_unique<Peer>(registry); replacement->feed(); replacement->finish();
            check(replacement->handshake.state() == ServerHandshake::State::accepted, "disconnect releases identity");
            auto independent = std::make_unique<Peer>(registry); auto m = hello(); m["endpoint"] = "modem2"; independent->feed(m); independent->finish();
            check(independent->handshake.state() == ServerHandshake::State::accepted, "second configured identity independent");
        } else if (name == "handshake_timeout") {
            peer->feed(hello(), 4999);
            check(!peer->handshake.sent(1, 5000) && peer->handshake.output().empty(), "reply drain cannot extend handshake deadline");
            auto replacement = std::make_unique<Peer>(registry); replacement->feed(); replacement->finish();
            check(replacement->handshake.state() == ServerHandshake::State::accepted, "timeout releases reservation");
        } else if (name == "handshake_tail") {
            auto bytes = frame(hello()); const auto count = bytes.size(); const auto duplicate = frame(hello()); bytes.insert(bytes.end(), duplicate.begin(), duplicate.end());
            auto result = peer->handshake.feed(bytes, 0, session);
            check(result.consumed == count, "pipelined tail retained by caller"); peer->finish();
            ServerInbox inbox(peer->lifecycle, peer->generation);
            check(inbox.feed(std::span(bytes).subspan(count), 1).status == ServerInbox::Status::failed, "duplicate hello rejected after handoff");
            peer->handshake.disconnect(); const auto newer = peer->lifecycle.begin(2);
            peer->handshake.disconnect(); check(peer->lifecycle.generation() == newer && peer->lifecycle.state() == ConnectionLifecycle::State::hello_wait, "stale cleanup safe");
        } else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
