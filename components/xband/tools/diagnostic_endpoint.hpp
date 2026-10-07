#pragma once
#include <local_service_endpoint.hpp>
// Development-only service: owns its parser, unlike the historical borrowed
// adapter. No game data, server account, card ledger or real modem is modified.
class OwnedDiagnosticEndpoint final : public xband::ServiceEndpoint {
public:
    explicit OwnedDiagnosticEndpoint(bool ipcp = true) : adapter_(parser_) { parser_.enableIPCP = ipcp; }
    bool transmit(uint8_t byte, unsigned frame) override { return adapter_.transmit(byte, frame); }
    void tick(unsigned frame) override { adapter_.tick(frame); }
    bool peek(uint8_t &byte) const override { return adapter_.peek(byte); }
    void consume() override { adapter_.consume(); }
    size_t pending() const override { return adapter_.pending(); }
    void reset() override { adapter_.reset(); }
private:
    LocalPPPProbe parser_;
    LocalServiceEndpoint adapter_;
};
