#pragma once
#include "diagnostic_endpoint.hpp"
#include <deque>
#include <string_view>

// VF diagnostic profile only, derived from xband_vf_dual_boot and local notes.
// NOT historical authentication: password content is neither stored nor checked.
class DiagnosticLoginEndpoint final : public xband::ServiceEndpoint {
public:
    enum class Phase { initial, greeting, username, password, ppp_start, ppp, failed };
    bool transmit(uint8_t byte, unsigned frame) override {
        observe(frame);
        switch (phase_) {
        case Phase::initial:
            if (byte != '\r') fail();
            emit("HELO"); greeting_frame_ = frame; phase_ = Phase::greeting; break;
        case Phase::username: {
            constexpr std::string_view expected = "Pstrn\n";
            if (byte != static_cast<uint8_t>(expected[username_count_])) fail();
            if (++username_count_ == expected.size()) { emit("Password:"); phase_ = Phase::password; }
            break;
        }
        case Phase::password:
            // Observed eight bytes + LF. Content is deliberately not retained.
            if (byte == '\n') {
                if (password_count_ != 8) fail();
                phase_ = Phase::ppp_start;
            } else {
                if (byte < 0x21 || byte > 0x7e || password_count_ == 8) fail();
                ++password_count_;
            }
            break;
        case Phase::ppp_start:
            if (byte != 0x7e) fail();
            phase_ = Phase::ppp;
            return ppp_.transmit(byte, frame);
        case Phase::ppp: return ppp_.transmit(byte, frame);
        default: fail();
        }
        return true;
    }
    void tick(unsigned frame) override {
        observe(frame);
        if (phase_ == Phase::greeting && frame - greeting_frame_ >= 60) {
            emit("login:"); phase_ = Phase::username;
        }
        if (phase_ == Phase::ppp) ppp_.tick(frame);
    }
    bool peek(uint8_t &byte) const override {
        if (!prompts_.empty()) { byte = prompts_.front(); return true; }
        return phase_ == Phase::ppp && ppp_.peek(byte);
    }
    void consume() override {
        if (!prompts_.empty()) prompts_.pop_front(); else ppp_.consume();
    }
    size_t pending() const override { return prompts_.size() + ppp_.pending(); }
    void reset() override {
        ppp_.reset(); prompts_.clear(); phase_ = Phase::initial;
        last_frame_ = greeting_frame_ = 0; username_count_ = password_count_ = 0;
    }
    Phase phase() const noexcept { return phase_; }
private:
    [[noreturn]] void fail() {
        prompts_.clear(); ppp_.reset(); phase_ = Phase::failed;
        throw std::runtime_error("diagnostic login profile mismatch"); // no credential bytes
    }
    void observe(unsigned frame) {
        if (phase_ == Phase::failed || frame < last_frame_) fail();
        last_frame_ = frame;
    }
    void emit(std::string_view text) {
        if (prompts_.size() + text.size() > 32) fail();
        for (auto byte : text) prompts_.push_back(static_cast<uint8_t>(byte));
    }
    OwnedDiagnosticEndpoint ppp_;
    std::deque<uint8_t> prompts_;
    Phase phase_ = Phase::initial;
    unsigned last_frame_ = 0, greeting_frame_ = 0;
    size_t username_count_ = 0, password_count_ = 0;
};
