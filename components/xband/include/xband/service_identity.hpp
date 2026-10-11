#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace xband {
// Only the two historical service identities are aliases. Peer/call names and
// arbitrary application endpoints must remain distinct.
constexpr std::string_view canonicalServiceEndpoint(std::string_view name) noexcept {
    if (name == "pb3-0") return "service-0";
    if (name == "pb3-1") return "service-1";
    return name;
}
inline std::string serviceEndpoint(unsigned side) {
    if (side > 1) throw std::invalid_argument("Service endpoint side");
    return "service-" + std::to_string(side);
}
inline std::string legacyServiceEndpoint(unsigned side) {
    if (side > 1) throw std::invalid_argument("Service endpoint side");
    return "pb3-" + std::to_string(side);
}
}
