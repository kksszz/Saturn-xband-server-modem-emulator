#pragma once
#include "local_phone_policy.hpp"

namespace xband {
// Internal pair routing is by endpoint, never by an account's phone number.
inline std::string peerEndpoint(unsigned side) {
    if(side>1)throw std::invalid_argument("Peer endpoint side");
    return "peer-"+std::to_string(side);
}
inline std::string pairedPeerEndpoint(unsigned side) {
    if(side>1)throw std::invalid_argument("Peer endpoint side");
    return peerEndpoint(1-side);
}
// Wire-v2 subscriber is separate from the ROM's registered account phone.
// Empty frontend input uses an endpoint-local ID; existing explicit values stay.
inline std::string localSubscriber(std::string_view text,unsigned side) {
    if(side>1)throw std::invalid_argument("Subscriber side");
    if(text.empty())return std::to_string(side+1);
    auto value=phoneDigits(text);
    if(value.empty()||value.size()>20)throw std::invalid_argument("Subscriber must contain 1..20 digits (hyphens allowed)");
    return value;
}
}
