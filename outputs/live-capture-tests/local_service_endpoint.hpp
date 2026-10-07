#pragma once
#include "service_endpoint.hpp"
#include "local_ppp_probe.hpp"

// Transitional adapter: borrows the existing configured parser so diagnostic
// profiles and status readers keep their established behavior. Owner outlives
// adapter; do not mutate parser.out while a consumer is using this interface.
class LocalServiceEndpoint final : public ServiceEndpoint {
public:
    explicit LocalServiceEndpoint(LocalPPPProbe &parser):parser(parser){}
    bool transmit(uint8_t byte,unsigned frame) override {parser.feed(byte,frame);return true;}
    void tick(unsigned frame) override {parser.tick(frame);}
    bool peek(uint8_t &byte) const override {
        if(parser.out.empty())return false;
        byte=parser.out.front();return true;
    }
    void consume() override {
        if(parser.out.empty())throw std::logic_error("consume empty service reply");
        parser.out.erase(parser.out.begin());
    }
    size_t pending() const override {return parser.out.size();}
    void reset() override {parser.resetSession();}
private:
    LocalPPPProbe &parser;
};
