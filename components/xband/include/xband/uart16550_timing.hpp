#pragma once
#include <cstdint>
#include <optional>
namespace xband {
// Explicit 16550-style diagnostic profile, NOT identification of Saturn hardware.
// Reference: https://www.ti.com/lit/ds/symlink/tl16c550c.pdf (baud generator/LCR).
// Caller supplies clock; no implicit oscillator assumption. RX character cadence
// models the selected full frame length, not waveform sampling or error flags.
inline std::optional<uint64_t> uart16550CharacterNanoseconds(uint32_t clock_hz,
    uint8_t dll, uint8_t dlm, uint8_t lcr) noexcept {
    const auto divisor = uint32_t{dll} | (uint32_t{dlm} << 8);
    if (!clock_hz || !divisor || (lcr & 0x40)) return std::nullopt; // break unsupported
    const unsigned data = 5 + (lcr & 3);
    const unsigned stop_half = !(lcr & 4) ? 2 : data == 5 ? 3 : 4;
    const unsigned half_bits = 2 + data * 2 + ((lcr & 8) ? 2 : 0) + stop_half;
    // <=24 half-bits *8 *65535 *1e9, safely within uint64_t.
    const uint64_t numerator = uint64_t{half_bits} * 8 * divisor * 1000000000ULL;
    return numerator / clock_hz + (numerator % clock_hz != 0);
}
}
