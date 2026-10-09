#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <xband/server_config.hpp>
#include <xband/server_session.hpp>
#include <xband/windows_token.hpp>
#include <algorithm>
#include <utility>

namespace xband::windows {
class Socket {
public:
    explicit Socket(SOCKET value = INVALID_SOCKET) noexcept : value_(value) {}
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
    ~Socket() { close(); }
    SOCKET get() const noexcept { return value_; }
    void close() noexcept { if (value_ != INVALID_SOCKET) { closesocket(value_); value_ = INVALID_SOCKET; } }
private:
    SOCKET value_;
};
class Runtime {
public:
    Runtime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock startup failed"); }
    ~Runtime() { WSACleanup(); }
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
};
inline void nonblocking(SOCKET socket) {
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled)) throw std::runtime_error("Nonblocking socket setup failed");
    // Small request/reply batches must not wait for Nagle/delayed-ACK pairing.
    const BOOL noDelay=TRUE;
    if(setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&noDelay),sizeof(noDelay)))
        throw std::runtime_error("TCP_NODELAY setup failed");
}
inline bool localUnicast(const IN_ADDR &address) {
    // Restrict loopback to the explicit default. Other addresses must appear on
    // an operational interface's unicast list; never infer local from RFC1918.
    if (ntohl(address.s_addr) == INADDR_LOOPBACK) return true;
    ULONG size = 16384;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (size > 1024 * 1024) return false;
        std::vector<uint64_t> storage((size + 7) / 8);
        auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
        const auto result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
        if (result == ERROR_BUFFER_OVERFLOW) continue;
        if (result != NO_ERROR) return false;
        for (auto *adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            for (auto *entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
                if (entry->Address.lpSockaddr && entry->Address.lpSockaddr->sa_family == AF_INET &&
                    reinterpret_cast<sockaddr_in *>(entry->Address.lpSockaddr)->sin_addr.s_addr == address.s_addr) return true;
            }
        }
        return false;
    }
    return false;
}

// Nonblocking, bounded owner-thread adapter. Caller schedules step() with host
// monotonic milliseconds (no internal sleep/thread/service). Destroy to stop.
class TcpHost {
public:
    static constexpr size_t max_clients = 32;
    enum class PortMode { configured, loopback_ephemeral_test };
    TcpHost(const protocol::ServerConfig &config, FrameClock clock, protocol::ServerSession::Factory factory,
            PortMode mode = PortMode::configured)
        : registry_(config.identities), clock_(clock), factory_(std::move(factory)),
          listener_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        unsigned frame = 0;
        if (!factory_ || !clock_.map(0, frame) || !config.port || !protocol::permittedBind(config.bind_address, config.allow_lan))
            throw std::invalid_argument("Invalid TCP host configuration");
        if (listener_.get() == INVALID_SOCKET) throw std::runtime_error("Socket creation failed");
        sockaddr_in address{}; address.sin_family = AF_INET;
        if (InetPtonA(AF_INET, config.bind_address.c_str(), &address.sin_addr) != 1 || !localUnicast(address.sin_addr))
            throw std::invalid_argument("Bind address is not a local active unicast address");
        if (mode == PortMode::loopback_ephemeral_test && config.bind_address != "127.0.0.1")
            throw std::invalid_argument("Ephemeral test mode is loopback only");
        address.sin_port = mode == PortMode::configured ? htons(config.port) : 0;
        BOOL exclusive = TRUE;
        if (setsockopt(listener_.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) ||
            bind(listener_.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener_.get(), 8))
            throw std::runtime_error("Exclusive TCP bind/listen failed");
        nonblocking(listener_.get()); int size = sizeof(address);
        if (getsockname(listener_.get(), reinterpret_cast<sockaddr *>(&address), &size)) throw std::runtime_error("Local port query failed");
        port_ = ntohs(address.sin_port);
        for (const auto &identity : config.identities) endpoint_names_.push_back(identity.name);
    }
    TcpHost(const TcpHost &) = delete;
    TcpHost &operator=(const TcpHost &) = delete;
    uint16_t port() const noexcept { return port_; }
    size_t clientCount() const noexcept { return clients_.size(); }
    bool stopped() const noexcept { return stopped_; }
    // Owner thread only. Keep the listener reserved but reject all sessions
    // while the simulated telephone line is absent. ON never restores a call.
    void setAccepting(bool accepting) noexcept {
        accepting_ = accepting;
        if (!accepting_) clients_.clear();
    }
    // Caller may select() over several hosts together, then call step().
    // The caller still owns heartbeat/deadline scheduling and the timeout.
    bool appendWaitSockets(fd_set &readable,fd_set &writable) const noexcept {
        if(stopped_||readable.fd_count>=FD_SETSIZE)return false;
        FD_SET(listener_.get(),&readable);
        for(const auto &peer:clients_){
            if(peer->cursor<peer->size||peer->session.needsPoll()||readable.fd_count>=FD_SETSIZE||writable.fd_count>=FD_SETSIZE)return false;
            FD_SET(peer->socket.get(),&readable);
            if(!peer->session.output().empty())FD_SET(peer->socket.get(),&writable);
        }
        return true;
    }
    // Owner-thread only, like step(). Returns an owned copy for subsequent UI
    // handoff; never exposes live session references or configured auth keys.
    // captured_at_ms is host observation time, NOT peer snapshot receipt time.
    nlohmann::json status() const {
        using Json = nlohmann::json;
        Json endpoints = Json::array();
        size_t pending = 0;
        for (const auto &client : clients_) if (!client->session.active()) ++pending;
        for (const auto &name : endpoint_names_) {
            Json row{{"endpoint", name}, {"connected", false}, {"call_active", false}, {"peer_snapshot", nullptr}};
            for (const auto &client : clients_) if (client->session.endpointName() == name) {
                row["connected"] = true;
                row["call_active"] = !client->session.call().empty();
                if (const auto *snapshot = client->session.snapshot()) row["peer_snapshot"] = *snapshot;
                break;
            }
            endpoints.push_back(std::move(row));
        }
        return Json{{"version", 1}, {"running", !stopped_}, {"captured_at_ms", std::to_string(last_time_)},
            {"pending_connections", pending}, {"endpoints", std::move(endpoints)}};
    }
    void stop() noexcept { clients_.clear(); listener_.close(); stopped_ = true; }
    bool step(uint64_t now) noexcept {
        if (stopped_) return false;
        if (started_ && now < last_time_) { stop(); return false; }
        started_ = true; last_time_ = now;
        try {
            for (unsigned i = 0; i < 4; ++i) {
                Socket accepted(accept(listener_.get(), nullptr, nullptr));
                if (accepted.get() == INVALID_SOCKET) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                    stop(); return false;
                }
                if (!accepting_ || clients_.size() == max_clients) continue; // RAII closes rejected sockets.
                nonblocking(accepted.get());
                clients_.push_back(std::make_unique<Peer>(std::move(accepted), registry_, now, clock_, factory_));
            }
            for (auto &client : clients_) client->step(now);
            std::erase_if(clients_, [](const auto &client) { return client->session.stopped(); });
            return true;
        } catch (...) { stop(); return false; }
    }
private:
    struct Peer {
        Socket socket;
        protocol::ServerSession session;
        std::array<uint8_t, 4096> input{};
        size_t size = 0, cursor = 0;
        std::string candidate = windowsRandomToken(), observed;
        Peer(Socket accepted, protocol::EndpointRegistry &registry, uint64_t now, FrameClock clock,
             const protocol::ServerSession::Factory &factory)
            : socket(std::move(accepted)), session(registry, now, windowsRandomToken(), clock, factory) {}
        bool poll(uint64_t now) {
            if (!session.poll(now, candidate)) return false;
            observeCall(); return true;
        }
        void observeCall() {
            const auto call = session.call();
            if (call != observed) {
                observed = call;
                if (!call.empty()) candidate = windowsRandomToken(); // next open, not a blocked retry
            }
        }
        void step(uint64_t now) noexcept {
            try {
                if (!poll(now)) return;
                if (cursor == size) {
                    cursor = size = 0;
                    const auto n = recv(socket.get(), reinterpret_cast<char *>(input.data()), static_cast<int>(input.size()), 0);
                    if (n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) { session.disconnect(); return; }
                    if (n > 0) size = static_cast<size_t>(n);
                }
                // At most four framed-message attempts per peer per step.
                for (unsigned i = 0; cursor < size && i < 4; ++i) {
                    const auto result = session.feed(std::span(input).first(size).subspan(cursor), now, candidate);
                    cursor += result.consumed;
                    observeCall(); // feed can finish a previously blocked open
                    if (!poll(now)) return;
                    if (!result.consumed) break; // hello output/backpressure; retain bounded tail
                }
                const auto output = session.output();
                if (!output.empty()) {
                    const int n = send(socket.get(), reinterpret_cast<const char *>(output.data()),
                        static_cast<int>(std::min<size_t>(4096, output.size())), 0);
                    if (n > 0) session.sent(static_cast<size_t>(n), now);
                    else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) session.disconnect();
                }
            } catch (...) { session.disconnect(); }
        }
    };
    Runtime runtime_; // constructed first, destroyed after every socket
    protocol::EndpointRegistry registry_; FrameClock clock_; protocol::ServerSession::Factory factory_;
    Socket listener_; std::vector<std::unique_ptr<Peer>> clients_;
    std::vector<std::string> endpoint_names_;
    uint16_t port_ = 0; uint64_t last_time_ = 0; bool started_ = false, stopped_ = false, accepting_ = true;
};
}
