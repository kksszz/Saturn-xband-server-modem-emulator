#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <xband/service_connection.hpp>
#include <chrono>
#include <deque>
#include <iostream>
#include <vector>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
constexpr std::string_view session = "0123456789abcdef0123456789abcdef";
constexpr std::string_view call1 = "11111111111111111111111111111111";
constexpr std::string_view call2 = "22222222222222222222222222222222";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { close(); }
    void close() { if (value != INVALID_SOCKET) { closesocket(value); value = INVALID_SOCKET; } }
    Socket() = default;
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
};
struct Winsock {
    Winsock() { WSADATA data{}; check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup"); }
    ~Winsock() { WSACleanup(); }
};
void nonblocking(SOCKET socket) { u_long enabled = 1; check(ioctlsocket(socket, FIONBIO, &enabled) == 0, "nonblocking"); }
void ready(SOCKET socket, bool write) {
    fd_set set; FD_ZERO(&set); FD_SET(socket, &set); timeval timeout{2, 0};
    check(select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout) == 1, "socket readiness timeout");
}
struct Pair {
    Winsock runtime; Socket client, server;
    Pair() {
        Socket listener; listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        check(listener.value != INVALID_SOCKET, "listener");
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
        check(bind(listener.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "loopback bind");
        int length = sizeof(address);
        check(getsockname(listener.value, reinterpret_cast<sockaddr *>(&address), &length) == 0 && address.sin_port != 0, "ephemeral port");
        check(listen(listener.value, 1) == 0, "listen"); nonblocking(listener.value);
        client.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); check(client.value != INVALID_SOCKET, "client"); nonblocking(client.value);
        const int connected = connect(client.value, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        check(connected == 0 || WSAGetLastError() == WSAEWOULDBLOCK, "connect start");
        ready(client.value, true); int error = 0; length = sizeof(error);
        check(getsockopt(client.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) == 0 && error == 0, "connect completion");
        ready(listener.value, false); server.value = accept(listener.value, nullptr, nullptr);
        check(server.value != INVALID_SOCKET, "accept"); nonblocking(server.value);
        // No listening socket remains during the exchange.
    }
};
struct Counts { unsigned input = 0, ticks = 0, resets = 0, destroyed = 0; };
struct Echo final : ServiceEndpoint {
    Counts &counts; std::deque<uint8_t> bytes;
    explicit Echo(Counts &c) : counts(c) {}
    ~Echo() override { ++counts.destroyed; }
    bool transmit(uint8_t byte, unsigned) override { ++counts.input; bytes.push_back(byte); return true; }
    void tick(unsigned) override { ++counts.ticks; }
    bool peek(uint8_t &byte) const override { if (bytes.empty()) return false; byte = bytes.front(); return true; }
    void consume() override { bytes.pop_front(); }
    size_t pending() const override { return bytes.size(); }
    void reset() override { ++counts.resets; bytes.clear(); }
};
uint64_t activate(ConnectionLifecycle &c) {
    const auto generation = c.begin(0);
    check(c.hello(generation, "test", 0) && c.authenticationCompleted(generation, true, session, 0), "fixture approval");
    return generation;
}
struct Harness {
    Pair sockets; Counts counts; ConnectionLifecycle lifecycle; uint64_t generation = activate(lifecycle);
    ServiceConnection host;
    explicit Harness(std::unique_ptr<ServiceEndpoint> service = {})
        : host(lifecycle, generation, "test", std::string(session),
               service ? std::move(service) : std::make_unique<Echo>(counts), FrameClock(1, 1000)) {}
    FrameQueue requests; MessageInbox replies; std::vector<Json> messages;
    uint64_t next = 2, response = 2, now = 0;
    std::string_view fresh_call = call1;
    uint64_t request(std::string type, Json body) {
        const auto text = Json{{"v", 2}, {"type", type}, {"endpoint", "test"}, {"session", session},
            {"id", std::to_string(next++)}, {"reply_to", nullptr}, {"body", body}}.dump();
        check(requests.enqueue(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size())) == FrameQueue::Result::ok, "request queued");
        return next - 1;
    }
    static size_t sendBytes(SOCKET socket, std::span<const uint8_t> bytes) {
        if (bytes.empty()) return 0;
        const int sent = send(socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(std::min<size_t>(17, bytes.size())), 0);
        if (sent == SOCKET_ERROR) { check(WSAGetLastError() == WSAEWOULDBLOCK, "send error"); return 0; }
        check(sent > 0, "send progress"); return static_cast<size_t>(sent);
    }
    void step() {
        ++now;
        if (const auto n = sendBytes(sockets.client.value, requests.peek())) check(requests.consume(n), "request consume");
        std::array<uint8_t, 113> bytes{};
        int n = recv(sockets.server.value, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
        if (n == SOCKET_ERROR) check(WSAGetLastError() == WSAEWOULDBLOCK, "server recv");
        else if (n == 0) host.disconnect();
        else {
            size_t used = 0;
            while (used < static_cast<size_t>(n)) {
                const auto result = host.feed(std::span(bytes).first(static_cast<size_t>(n)).subspan(used), now);
                check(result.consumed > 0 && result.status != ServerInbox::Status::failed, "server frame"); used += result.consumed;
                check(host.poll(now, fresh_call) != ServiceConnection::Outcome::failed, "dispatch");
            }
        }
        check(host.poll(now, fresh_call) != ServiceConnection::Outcome::failed, "pump");
        if (const auto sent = sendBytes(sockets.server.value, host.output())) check(host.sent(sent), "output consume");
        n = recv(sockets.client.value, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
        if (n == SOCKET_ERROR) check(WSAGetLastError() == WSAEWOULDBLOCK, "client recv");
        else {
            check(n > 0, "unexpected EOF"); size_t used = 0;
            while (used < static_cast<size_t>(n)) {
                const auto result = replies.feed(std::span(bytes).first(static_cast<size_t>(n)).subspan(used));
                check(result.consumed > 0 && result.status != MessageInbox::Status::failed, "client frame"); used += result.consumed;
                if (const auto *message = replies.peek()) {
                    check((*message)["id"] == std::to_string(response++), "shared response sequence");
                    messages.push_back(*message); replies.consume();
                }
            }
        }
    }
    void until(size_t count) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (messages.size() < count && std::chrono::steady_clock::now() < end) { step(); Sleep(1); }
        check(messages.size() == count, "reply count/deadline");
    }
    void open() { request("open", {{"target", "local-service"}, {"subscriber", "0123456789"}}); until(1); check(host.call() == call1, "active call"); }
};
