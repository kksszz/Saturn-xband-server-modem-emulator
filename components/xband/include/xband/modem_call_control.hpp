#pragma once
#include <xband/client_control.hpp>
#include <string_view>

namespace xband {
// Limited parsed-AT call adapter, not a full modem command interpreter.
// Owns neither TCP nor UART. The ClientControl reference must remain alive.
// Subscriber is the locally configured identity, NOT the dialed service number.
class ModemCallControl {
public:
    enum class State { idle, dialing, connected, closing, stopped };
    ModemCallControl(protocol::ClientControl &client, std::string subscriber, std::string service_number)
        : client_(client), subscriber_(std::move(subscriber)), number_(std::move(service_number)) {
        if (!digits(subscriber_) || !digits(number_)) throw std::invalid_argument("invalid modem number");
    }
    ModemCallControl(const ModemCallControl &) = delete;
    ModemCallControl &operator=(const ModemCallControl &) = delete;
    // Caller supplies normalized complete command, without CR. false = not handled.
    bool command(std::string_view cmd, uint64_t now) {
        if (state_ == State::stopped) return false;
        if (cmd == "AT") { emit("\r\nOK\r\n"); return true; }
        if (cmd == "ATH0" || cmd == "ATZ" || cmd == "ATZ0") {
            reply_.clear();
            if (state_ == State::idle) { emit("\r\nOK\r\n"); return true; }
            // Pending work cannot be retracted safely. End the connection.
            if (state_ != State::connected || client_.advancing()) { stop(); emit("\r\nOK\r\n"); return true; }
            if (client_.close(now) != protocol::Admission::queued) { stop(); emit("\r\nNO CARRIER\r\n"); return true; }
            state_ = State::closing; return true;
        }
        std::string_view dial;
        if (cmd.starts_with("ATDT")) dial = cmd.substr(4);
        else if (cmd.starts_with("ATS91=15S92=15DT")) dial = cmd.substr(std::string_view("ATS91=15S92=15DT").size());
        else return false;
        if (state_ != State::idle || !digits(dial)) { emit("\r\nERROR\r\n"); return true; }
        // Exact configured service route only: no real telephone calls or peer routing.
        if (dial != number_) { emit("\r\nNO CARRIER\r\n"); return true; }
        if (client_.open(subscriber_, now) != protocol::Admission::queued) { emit("\r\nNO CARRIER\r\n"); return true; }
        state_ = State::dialing; return true;
    }
    void poll() {
        if (state_ == State::stopped) return;
        const auto peer = client_.state();
        if (peer == protocol::ClientControl::State::stopped) {
            reply_.clear(); stop(); emit("\r\nNO CARRIER\r\n");
        } else if (state_ == State::dialing && peer == protocol::ClientControl::State::service) {
            state_ = State::connected; emit("\r\nCONNECT 14400\r\n");
        } else if (state_ == State::closing && peer == protocol::ClientControl::State::idle) {
            state_ = State::idle; emit("\r\nOK\r\n");
        }
    }
    bool carrier() const noexcept { return state_ == State::connected; }
    State state() const noexcept { return state_; }
    std::string_view reply() const noexcept { return reply_; }
    void consume(size_t count) {
        if (count > reply_.size()) throw std::out_of_range("modem reply consumption");
        reply_.erase(0, count);
    }
private:
    static bool digits(std::string_view text) {
        return !text.empty() && text.size() <= 20 && text.find_first_not_of("0123456789") == std::string_view::npos;
    }
    void stop() noexcept { client_.disconnect(); state_ = State::stopped; reply_.clear(); }
    void emit(std::string_view text) {
        if (reply_.size() + text.size() > 256) { stop(); return; }
        reply_.append(text);
    }
    protocol::ClientControl &client_;
    std::string subscriber_, number_, reply_;
    State state_ = State::idle;
};
}
