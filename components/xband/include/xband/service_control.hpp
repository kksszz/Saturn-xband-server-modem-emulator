#pragma once
#include <xband/server_inbox.hpp>
#include <xband/service_endpoint.hpp>
#include <xband/session_outbox.hpp>

namespace xband::protocol {
// One owner-thread controller per connection generation. All referenced objects
// belong to that generation and outlive this object. Output IDs are assigned by
// the same SessionOutbox used by data and other control producers.
class ServiceControl {
public:
    enum class Outcome { idle, handled, blocked, other_handler, failed };
    ServiceControl(ServerInbox &inbox, ConnectionLifecycle &connection, uint64_t generation,
                   SessionOutbox &output, ServiceEndpoint &service) noexcept
        : inbox_(inbox), connection_(connection), generation_(generation), output_(output), service_(service) {}
    std::string_view call() const noexcept { return call_; }
    std::string_view subscriber() const noexcept { return subscriber_; }
    // For open only, the host supplies a fresh unique random call token. Never
    // accept this value from the requester. There is no random generator here.
    Outcome process(std::string_view new_call = {}) noexcept {
        if (failed_ || connection_.generation() != generation_ || connection_.state() != ConnectionLifecycle::State::active)
            return Outcome::failed;
        const auto *request = inbox_.peek();
        if (!request) return Outcome::idle;
        if (inbox_.route() != ServerInbox::Route::service && inbox_.route() != ServerInbox::Route::ping)
            return Outcome::other_handler;
        try {
            using Json = nlohmann::json;
            const auto &type = (*request)["type"].get_ref<const std::string &>();
            Json body = Json::object(); std::string response;
            std::string next_call = call_, next_subscriber = subscriber_;
            bool reset = false;
            if (type == "ping") response = "pong";
            else if (type == "open" && call_.empty()) {
                if (!token(new_call)) return fail();
                response = "open_ok"; body["call"] = new_call;
                next_call = new_call; next_subscriber = (*request)["body"]["subscriber"].get<std::string>(); reset = true;
            } else if (type == "close" && !call_.empty() && (*request)["body"]["call"] == call_) {
                response = "close_ok"; body["call"] = call_;
                next_call.clear(); next_subscriber.clear(); reset = true;
            } else {
                response = "error";
                body = {{"code", type == "open" ? "BUSY" : "STALE_CALL"},
                    {"message", type == "open" ? "Service call already open" : "Call is not active"},
                    {"scope", "call"}, {"call", type == "open" ? Json(call_) : (*request)["body"]["call"]}};
            }
            const auto result = output_.control(response, std::move(body), (*request)["id"]);
            if (result == Admission::queue_full) return Outcome::blocked;
            if (result != Admission::queued) return fail();
            // No socket pumping/reentrancy during this transaction. On reset
            // failure queued success is discarded before it can reach the peer.
            if (reset) service_.reset();
            call_.swap(next_call); subscriber_.swap(next_subscriber);
            inbox_.consume(); return Outcome::handled;
        } catch (...) { return fail(); }
    }
private:
    Outcome fail() noexcept {
        failed_ = true; call_.clear(); subscriber_.clear(); output_.discard();
        if (connection_.generation() == generation_) connection_.close();
        // Host must dispose/reset the service before reuse; do not retry a
        // throwing reset here, or leak internal exception text into protocol.
        return Outcome::failed;
    }
    ServerInbox &inbox_;
    ConnectionLifecycle &connection_;
    uint64_t generation_;
    SessionOutbox &output_;
    ServiceEndpoint &service_;
    std::string call_, subscriber_;
    bool failed_ = false;
};
}
