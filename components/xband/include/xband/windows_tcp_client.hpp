#pragma once
#include <xband/windows_tcp_host.hpp> // shared Winsock RAII and address policy
#include <xband/client_control.hpp>

namespace xband::windows {
struct ClientConfig {
    std::string server_address = "127.0.0.1";
    uint16_t port = 0;
    bool allow_lan = false;
    std::string endpoint, key;
    uint64_t clock_hz = 0;
};
inline bool permittedServer(std::string_view address, bool lan) noexcept {
    return address == "127.0.0.1" || (lan && !address.starts_with("127.") && protocol::permittedBind(address, true));
}

// Owner-thread nonblocking transport, one object per connection. No worker,
// sleep, DNS, firewall change, reconnect or guest-clock progression. step() must
// continue during guest pause to service deadlines/heartbeats. It never calls
// transmit/advance/consumeReceived itself: those are the emulator's decisions.
class TcpClient {
public:
    enum class Failure { none, connect_timeout, io, protocol, clock_reversed, internal };
    TcpClient(ClientConfig config, uint64_t now)
        : config_(std::move(config)), started_(now), last_time_(now) {
        if (!permittedServer(config_.server_address, config_.allow_lan) || !config_.port ||
            !protocol::endpoint(config_.endpoint) || config_.key.size() != 64 ||
            !protocol::token(std::string_view(config_.key).substr(0, 32)) ||
            !protocol::token(std::string_view(config_.key).substr(32)) ||
            !config_.clock_hz || config_.clock_hz > 1000000000)
            throw std::invalid_argument("Invalid TCP client configuration");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config_.port);
        if (InetPtonA(AF_INET, config_.server_address.c_str(), &address.sin_addr) != 1)
            throw std::invalid_argument("Invalid TCP server address");
        socket_ = std::make_unique<Socket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (socket_->get() == INVALID_SOCKET) throw std::runtime_error("Client socket creation failed");
        nonblocking(socket_->get());
        const int result = connect(socket_->get(), reinterpret_cast<sockaddr *>(&address), sizeof(address));
        if (result == 0) connected(now);
        else if (WSAGetLastError() != WSAEWOULDBLOCK) fail(Failure::io);
    }
    TcpClient(const TcpClient &) = delete;
    TcpClient &operator=(const TcpClient &) = delete;
    ~TcpClient() { stop(); }
    bool stopped() const noexcept { return stopped_; }
    bool connecting() const noexcept { return !stopped_ && !control_; }
    Failure failure() const noexcept { return failure_; }
    static const char *failureName(Failure why) noexcept {
        switch(why){
        case Failure::none:return "none";
        case Failure::connect_timeout:return "connect timeout";
        case Failure::io:return "socket I/O or EOF";
        case Failure::protocol:return "protocol or liveness check";
        case Failure::clock_reversed:return "host clock reversed";
        case Failure::internal:return "internal exception";
        }
        return "unknown transport failure";
    }
    // Owner-thread readiness registration. No ownership transfer or guest work.
    // false means local buffered work or insufficient fd_set space: step again.
    bool appendWaitSockets(fd_set &readable,fd_set &writable) const noexcept {
        if(stopped_||cursor_<size_||readable.fd_count>=FD_SETSIZE||writable.fd_count>=FD_SETSIZE)return false;
        FD_SET(socket_->get(),&readable);
        if(!control_||!control_->output().empty())FD_SET(socket_->get(),&writable);
        return true;
    }
    // Borrowed owner-thread pointer, invalidated by stop/destruction. A non-null
    // pointer is NOT proof of hello success: inspect its state before opening.
    protocol::ClientControl *control() noexcept { return stopped_ ? nullptr : control_.get(); }
    void stop() noexcept {
        if (control_) control_->disconnect();
        control_.reset(); socket_.reset(); config_.key.clear(); size_ = cursor_ = 0; stopped_ = true;
    }
    bool step(uint64_t now) noexcept {
        if (stopped_) return false;
        if (now < last_time_) return fail(Failure::clock_reversed);
        last_time_ = now;
        try {
            if (!control_) {
                if (now - started_ >= protocol::ConnectionLifecycle::handshake_ms) return fail(Failure::connect_timeout);
                fd_set writable, errors; FD_ZERO(&writable); FD_ZERO(&errors);
                FD_SET(socket_->get(), &writable); FD_SET(socket_->get(), &errors);
                timeval timeout{};
                const int ready = select(0, nullptr, &writable, &errors, &timeout);
                if (ready == SOCKET_ERROR) return fail(Failure::io);
                if (!ready) return true;
                int error = 0, length = sizeof(error);
                if (getsockopt(socket_->get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) || error)
                    return fail(Failure::io);
                connected(now);
            }
            if (!control_->poll(now)) return fail(Failure::protocol);
            // Send before receive so hello output is accounted before hello_ok.
            const auto outgoing = control_->output();
            if (!outgoing.empty()) {
                const int count = send(socket_->get(), reinterpret_cast<const char *>(outgoing.data()),
                    static_cast<int>(std::min<size_t>(4096, outgoing.size())), 0);
                if (count > 0) { if (!control_->sent(static_cast<size_t>(count))) return fail(Failure::protocol); }
                else if (count == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return fail(Failure::io);
            }
            if (cursor_ == size_) {
                cursor_ = size_ = 0;
                const int count = recv(socket_->get(), reinterpret_cast<char *>(input_.data()), static_cast<int>(input_.size()), 0);
                if (count == 0 || (count == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) return fail(Failure::io);
                if (count > 0) size_ = static_cast<size_t>(count);
            }
            for (unsigned attempt = 0; cursor_ < size_ && attempt < 4; ++attempt) {
                const auto result = control_->feed(std::span(input_).first(size_).subspan(cursor_), now);
                if (result.status == protocol::MessageInbox::Status::failed) return fail(Failure::protocol);
                cursor_ += result.consumed;
                if (!result.consumed) break; // retained tail, resume after output drains
            }
            return true;
        } catch (...) { return fail(Failure::internal); }
    }
private:
    void connected(uint64_t now) {
        control_ = std::make_unique<protocol::ClientControl>(config_.endpoint, config_.key, config_.clock_hz, now);
        config_.key.clear(); // no reconnect; no retained configuration credential needed
    }
    bool fail(Failure why) noexcept { failure_ = why; stop(); return false; }
    Runtime runtime_; // must outlive every socket
    ClientConfig config_; std::unique_ptr<Socket> socket_;
    std::unique_ptr<protocol::ClientControl> control_;
    std::array<uint8_t, 4096> input_{}; size_t size_ = 0, cursor_ = 0;
    uint64_t started_, last_time_; bool stopped_ = false; Failure failure_ = Failure::none;
};
}
