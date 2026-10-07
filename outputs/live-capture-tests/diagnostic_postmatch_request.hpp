#pragma once
#include <xband/local_phone_policy.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <span>

namespace diagnostic {
// One observed VF REMIX post-call pair, captured after a completed generation-1
// peer connection. This deliberately is not a general XBAND result decoder.
inline uint8_t observedPostMatchRequest(std::span<const uint8_t> bytes) {
    if (bytes.size() != 577 && (bytes.size() < 592 || bytes.size() > 596) && bytes.size() != 664) return 0;
    constexpr std::array<uint8_t, 6> prefix{0x1f, 0x74, 0x6a, 0x30, 0x34, 0x0b};
    if (!std::equal(prefix.begin(), prefix.end(), bytes.begin())) return 0;
    try {
        xband::registrationPhone(bytes);
        const auto code = xband::registrationOffset(bytes, 160);
        const auto card = xband::registrationOffset(bytes, 135);
        const auto records = xband::registrationOffset(bytes, 184);
        if (code >= bytes.size() || code == 0 || bytes[code - 1] != 0x0e ||
            card + 8 > bytes.size() || bytes[card] != 0x1e || bytes[card + 1] != 0 ||
            !std::all_of(bytes.begin() + card + 4, bytes.begin() + card + 8, [](uint8_t b) { return b == 0; }))
            return 0;
        auto recordAt = [&](size_t at) {
            return at + 9 <= bytes.size() && bytes[at] == 0x15 && bytes[at + 1] == 0 && bytes[at + 2] == 1 &&
                   bytes[at + 3] == 0 && bytes[at + 4] == 1 && bytes[at + 5] == 0 && bytes[at + 6] == 0 &&
                   bytes[at + 7] == 1 && bytes[at + 8] == 0x14;
        };
        if (bytes.size() == 596 && bytes[code] == 2) {
            constexpr std::array<uint8_t, 13> tail{0x24, 0x26, 0x26, 0x1b, 0, 0, 0, 1, 0x29, 0, 0, 0, 0};
            if (records + 4 + 9 + 276 + 3 > bytes.size() ||
                !std::all_of(bytes.begin() + records, bytes.begin() + records + 4, [](uint8_t b) { return b == 0; }) ||
                !recordAt(records + 4) ||
                bytes[records + 4 + 9 + 276] != 0x16 || bytes[records + 4 + 9 + 276 + 1] != 0 ||
                bytes[records + 4 + 9 + 276 + 2] != 1 ||
                !std::equal(tail.begin(), tail.end(), bytes.end() - tail.size()))
                return 0;
            return 2;
        }
        if (bytes.size() == 664 && bytes[code] == 3) {
            constexpr std::array<uint8_t, 7> tail{0x21, 0, 0x29, 0, 0, 0, 0};
            const auto after = records + 9 + 276;
            if (!recordAt(records) || after + 6 > bytes.size() ||
                bytes[after] != 0x16 || bytes[after + 1] != 0 || bytes[after + 2] != 0 ||
                bytes[after + 3] != 0x1d || bytes[after + 4] != 0 || bytes[after + 5] != 1 ||
                !std::equal(tail.begin(), tail.end(), bytes.end() - tail.size()))
                return 0;
            return 3;
        }
        if (bytes.size() >= 592 && bytes.size() <= 596 && bytes[code] == 3) {
            // Observed VF sessions submitted one 16-entry and no 1D entries.
            // Observed 592/593/596-byte forms differ in embedded player-name
            // length (3/4/7). Code 02 at 596 bytes is handled above.
            constexpr std::array<uint8_t, 13> tail{0x24, 0x26, 0x26, 0x1b, 0, 0, 0, 1, 0x29, 0, 0, 0, 0};
            const auto after = records + 9 + 276;
            const size_t nameLength = bytes.size() - 589;
            if (!recordAt(records) || after + 18 + nameLength > bytes.size() ||
                bytes[after] != 0x16 || bytes[after + 1] != 0 || bytes[after + 2] != 1 ||
                bytes[after + 3] != 0 ||
                !std::all_of(bytes.begin() + after + 4, bytes.begin() + after + 13,
                             [](uint8_t b) { return b == 0xff; }) ||
                bytes[after + 13] != 0 || bytes[after + 14] != nameLength ||
                bytes[after + 14 + nameLength] != 0 || bytes[after + 15 + nameLength] != 0x1d ||
                bytes[after + 16 + nameLength] != 0 || bytes[after + 17 + nameLength] != 0 ||
                !std::equal(tail.begin(), tail.end(), bytes.end() - tail.size()))
                return 0;
            return 3;
        }
        if (bytes.size() == 577 && bytes[code] == 3) {
            // Third observed VF session: caller has no 16 or 1D entries.
            constexpr std::array<uint8_t, 13> tail{0x24, 0x26, 0x26, 0x1b, 0, 0, 0, 1, 0x29, 0, 0, 0, 0};
            const auto after = records + 9 + 276;
            if (!recordAt(records) || after + 6 > bytes.size() ||
                bytes[after] != 0x16 || bytes[after + 1] != 0 || bytes[after + 2] != 0 ||
                bytes[after + 3] != 0x1d || bytes[after + 4] != 0 || bytes[after + 5] != 0 ||
                !std::equal(tail.begin(), tail.end(), bytes.end() - tail.size()))
                return 0;
            return 3;
        }
    } catch (const std::exception &) {
        return 0;
    }
    return 0;
}
}
