#include "authenticated_fixture.hpp"
#include <set>
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; EndpointRegistry registry({{"test", configured_key}});
        auto c = std::make_unique<Connection>(registry, name == "tcp_factory_failure");
        if (name == "random_tokens") {
            std::set<std::string> generated;
            for (unsigned i = 0; i < 256; ++i) {
                const auto value = windowsRandomToken();
                check(token(value) && generated.insert(value).second, "sample format/no duplicates");
            }
        } else if (name == "tcp_stale_session") {
            c->hello(); c->replies(1); const auto old_session = c->client_session;
            c->server.disconnect(); c->step();
            auto retry = std::make_unique<Connection>(registry); retry->hello(); retry->replies(1);
            check(retry->client_session != old_session, "new connection has fresh session");
            retry->request("ping", Json::object(), old_session); retry->until([&] { return retry->eof; });
            check(retry->messages.size() == 1 && retry->counts.input == 0 && retry->counts.destroyed == 1, "old session rejected and service disposed");
        } else if (name == "tcp_hello_data") {
            c->hello(); c->replies(1);
            check(c->messages[0]["type"] == "hello_ok" && c->messages[0]["body"]["new_session"].get<std::string>() == c->issued_session, "wire permission reply");
            c->request("open", {{"target", "local-service"}, {"subscriber", "0001"}}); c->replies(2);
            c->request("data", {{"call", c->call_token}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}}); c->replies(3);
            c->request("advance", {{"call", c->call_token}, {"tick", "1000"}}); c->replies(6);
            check(c->messages[3]["body"]["payload_b64"] == "QUJD" && c->messages[5]["type"] == "advance_ok" &&
                c->counts.input == 3 && c->constructions == 1, "authenticated echo path");
            c->request("close", {{"call", c->call_token}, {"reason", "hangup"}}); c->replies(7);
            check(shutdown(c->sockets.client.value, SD_SEND) == 0, "EOF shutdown"); c->until([&] { return c->eof; });
            check(c->counts.destroyed == 1, "EOF disposes service");
        } else if (name == "tcp_hello_denied") {
            c->hello(std::string(64, 'b')); c->until([&] { return c->eof; });
            check(c->messages.empty() && c->constructions == 0, "bad key no service/reply");
        } else if (name == "tcp_hello_duplicate") {
            c->hello(); c->replies(1);
            auto duplicate = std::make_unique<Connection>(registry); duplicate->hello(); duplicate->until([&] { return duplicate->eof; });
            check(duplicate->constructions == 0 && duplicate->messages.empty(), "duplicate rejected");
            c->server.disconnect(); c->step();
            auto retry = std::make_unique<Connection>(registry); retry->hello(); retry->replies(1);
            check(retry->messages[0]["type"] == "hello_ok", "released reservation reusable");
        } else if (name == "tcp_hello_tail") {
            c->hello(); c->request("ping", Json::object(), c->issued_session); c->replies(2);
            check(c->messages[0]["type"] == "hello_ok" && c->messages[1]["type"] == "pong" &&
                c->messages[1]["reply_to"] == "2" && c->constructions == 1, "pipelined tail dispatched after permission");
        } else if (name == "tcp_factory_failure") {
            c->hello(); c->until([&] { return c->eof; }); check(c->constructions == 1 && c->server.stopped(), "factory failure closes socket");
            auto retry = std::make_unique<Connection>(registry); retry->hello(); retry->replies(1);
            check(retry->messages[0]["type"] == "hello_ok", "factory failure releases identity");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
