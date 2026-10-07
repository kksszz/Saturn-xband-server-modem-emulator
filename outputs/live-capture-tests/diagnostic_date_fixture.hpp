#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace diagnostic {
// Explicit, isolated VF fixture. Numeric ROM fields display 26/10/5;
// calendar epoch and the second scalar's general meaning are unverified.
// Zero scalar matches the paired observation, not a host-clock conversion.
struct DateFixture {
    uint32_t date=0x07ea9280, scalar=0;
    std::array<uint8_t,9> wire() const {
        if(date!=0x07ea9280 || scalar!=0) throw std::runtime_error("Unsupported date fixture fields");
        return {4,7,0xea,0x92,0x80,0,0,0,0};
    }
};
inline std::optional<DateFixture> parseDateFixture(const char *value) {
    if(!value || !*value) return std::nullopt;
    if(std::string_view(value)!="07EA9280:00000000")
        throw std::runtime_error("XBAND_DATE_FIXTURE supports only explicit 07EA9280:00000000; synthetic, not real time");
    return DateFixture{};
}
}
