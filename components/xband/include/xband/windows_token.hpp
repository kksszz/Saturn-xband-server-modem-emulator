#pragma once
// Windows adapter only. Portable protocol headers do not include this file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <stdexcept>
#include <string>

namespace xband {
// 128 random bits, canonical lowercase hexadecimal. Fail closed on OS error;
// never fall back to time, process IDs, counters or deterministic PRNGs.
inline std::string windowsRandomToken() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("System random token generation failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        result[2 * i] = hex[bytes[i] >> 4]; result[2 * i + 1] = hex[bytes[i] & 15];
    }
    return result;
}
}
