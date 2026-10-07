#pragma once
#include "loopback_fixture.hpp"
#include <xband/server_session.hpp>
#include <xband/windows_token.hpp>
const std::string configured_key(64, 'a');
struct Connection {
    Pair sockets; Counts counts; unsigned constructions = 0;
    std::string issued_session = windowsRandomToken(), call_token = windowsRandomToken(), client_session;
    ServerSession server;
    FrameQueue requests; MessageInbox decoder; std::vector<Json> messages;
    std::vector<uint8_t> tail;
    uint64_t next = 1, response = 1;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool eof = false;
    explicit Connection(EndpointRegistry &registry, bool throw_factory = false, ServerSession::Factory factory = {})
        : server(registry, 0, issued_session, FrameClock(1, 1000),
            [this, throw_factory, factory = std::move(factory)](uint64_t hz) -> std::unique_ptr<ServiceEndpoint> {
                check(hz == 1000, "declared clock reaches factory"); ++constructions;
                if (throw_factory) throw std::runtime_error("fixture failure");
                return factory ? factory(hz) : std::make_unique<Echo>(counts);
            }) {}
    uint64_t now() const { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); }
    uint64_t request(std::string type, Json body, std::string_view token_value = {}) {
        if (type != "hello" && token_value.empty()) token_value = client_session;
        if (type == "open") call_token = windowsRandomToken();
        const auto text = Json{{"v", 2}, {"type", type}, {"endpoint", "test"}, {"session", token_value},
            {"id", std::to_string(next++)}, {"reply_to", nullptr}, {"body", body}}.dump();
        check(requests.enqueue(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size())) == FrameQueue::Result::ok, "queue wire request");
        return next - 1;
    }
    void hello(std::string key = configured_key) {
        request("hello", {{"client", "socket-test"}, {"clock_hz", 1000}, {"auth_key", key}}, "");
    }
    void step() {
        const auto time = now();
        if (!eof && !server.stopped()) {
            if (auto n = Harness::sendBytes(sockets.client.value, requests.peek())) check(requests.consume(n), "consume request");
        }
        std::array<uint8_t, 113> bytes{};
        if (!server.stopped()) {
            if (tail.empty()) {
                const int n = recv(sockets.server.value, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
                if (n == SOCKET_ERROR) check(WSAGetLastError() == WSAEWOULDBLOCK, "server receive");
                else if (n == 0) server.disconnect();
                else tail.assign(bytes.begin(), bytes.begin() + n);
            }
            // Retain any post-hello bytes while the reply is partially sent.
            if (!tail.empty() && !server.stopped()) {
                const auto result = server.feed(tail, time, call_token);
                tail.erase(tail.begin(), tail.begin() + result.consumed);
            }
            if (server.poll(time, call_token)) {
                if (const auto n = Harness::sendBytes(sockets.server.value, server.output())) server.sent(n, time);
            }
        }
        if (server.stopped()) { sockets.server.close(); tail.clear(); }
        if (!eof) {
            const int n = recv(sockets.client.value, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (n == SOCKET_ERROR) check(WSAGetLastError() == WSAEWOULDBLOCK, "client receive");
            else if (n == 0) eof = true;
            else {
                size_t used = 0;
                while (used < static_cast<size_t>(n)) {
                    const auto result = decoder.feed(std::span(bytes).first(static_cast<size_t>(n)).subspan(used));
                    check(result.consumed > 0 && result.status != MessageInbox::Status::failed, "reply decode"); used += result.consumed;
                    if (const auto *m = decoder.peek()) {
                        check((*m)["id"] == std::to_string(response++), "hello/service ID continuity");
                        if ((*m)["type"] == "hello_ok") client_session = (*m)["body"]["new_session"].get<std::string>();
                        messages.push_back(*m); decoder.consume();
                    }
                }
            }
        }
    }
    template<class Predicate> void until(Predicate done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!done() && std::chrono::steady_clock::now() < deadline) { step(); Sleep(1); }
        check(done(), "TCP exchange deadline");
    }
    void replies(size_t count) { until([&] { return messages.size() >= count; }); check(messages.size() == count, "reply count"); }
};
