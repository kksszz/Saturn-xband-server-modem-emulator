#pragma once
#include "loopback_fixture.hpp"
#include <xband/windows_tcp_host.hpp>
using Host = xband::windows::TcpHost;
ServerConfig configuration() {
    const auto parsed = parseServerConfig(Json{{"version", 1}, {"port", 58133}, {"identities", Json::array({
        {{"endpoint", "test"}, {"auth_key", std::string(64, 'a')}}})}}.dump());
    check(bool(parsed), "configuration parsed"); return *parsed.config;
}
struct Client {
    Socket socket; FrameQueue outgoing; MessageInbox incoming; std::vector<Json> messages;
    uint64_t next = 1; std::string token_value;
    explicit Client(uint16_t port) {
        socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); check(socket.value != INVALID_SOCKET, "client socket"); nonblocking(socket.value);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        const int connected = connect(socket.value, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        check(connected == 0 || WSAGetLastError() == WSAEWOULDBLOCK, "connect"); ready(socket.value, true);
        int error = 0, size = sizeof(error);
        check(getsockopt(socket.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) == 0 && error == 0, "connect result");
    }
    void request(std::string type, Json body, Json reply = nullptr) {
        auto text = Json{{"v", 2}, {"type", type}, {"endpoint", "test"}, {"session", token_value}, {"id", std::to_string(next++)},
            {"reply_to", reply}, {"body", body}}.dump();
        check(outgoing.enqueue(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size())) == FrameQueue::Result::ok, "request");
    }
    void exchange(Host &host, uint64_t &time, size_t count) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (messages.size() < count && std::chrono::steady_clock::now() < end) {
            if (const auto n = Harness::sendBytes(socket.value, outgoing.peek())) outgoing.consume(n);
            check(host.step(++time), "host step"); std::array<uint8_t, 4096> bytes{};
            const auto n = recv(socket.value, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (n == SOCKET_ERROR) check(WSAGetLastError() == WSAEWOULDBLOCK, "read");
            else {
                check(n > 0, "unexpected close"); size_t cursor = 0;
                while (cursor < static_cast<size_t>(n)) {
                    const auto result = incoming.feed(std::span(bytes).first(static_cast<size_t>(n)).subspan(cursor));
                    check(result.consumed && result.status != MessageInbox::Status::failed, "reply framing"); cursor += result.consumed;
                    if (const auto *m = incoming.peek()) {
                        check((*m)["id"] == std::to_string(messages.size() + 1), "reply sequence");
                        if ((*m)["type"] == "hello_ok") token_value = (*m)["body"]["new_session"].get<std::string>();
                        messages.push_back(*m); incoming.consume();
                    }
                }
            }
            Sleep(1);
        }
        check(messages.size() == count, "exchange completed");
    }
};
