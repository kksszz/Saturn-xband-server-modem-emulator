#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <string>

namespace xband::protocol {
inline bool encodeBase64(std::span<const uint8_t> input, std::string &output) {
    if (input.empty() || input.size() > 4096) return false;
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded; encoded.reserve((input.size() + 2) / 3 * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        const unsigned a = input[i];
        const unsigned b = i + 1 < input.size() ? input[i + 1] : 0;
        const unsigned c = i + 2 < input.size() ? input[i + 2] : 0;
        encoded.push_back(alphabet[a >> 2]); encoded.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        encoded.push_back(i + 1 < input.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        encoded.push_back(i + 2 < input.size() ? alphabet[c & 63] : '=');
    }
    output.swap(encoded); return true;
}
inline int base64Digit(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// Standard padded canonical Base64; data chunks are 1..4096 decoded bytes.
// Reject whitespace, URL alphabet, missing padding and nonzero padding bits.
inline bool base64Size(std::string_view input, size_t &size) noexcept {
    if (input.empty() || input.size() > 5464 || input.size() % 4 != 0) return false;
    const size_t padding = input.back() == '=' ? (input[input.size() - 2] == '=' ? 2 : 1) : 0;
    const size_t count = input.size() / 4 * 3 - padding;
    if (count > 4096) return false;
    for (size_t i = 0; i < input.size() - padding; ++i) if (base64Digit(input[i]) < 0) return false;
    if (padding == 2 && (base64Digit(input[input.size() - 3]) & 15) != 0) return false;
    if (padding == 1 && (base64Digit(input[input.size() - 2]) & 3) != 0) return false;
    size = count; return true;
}
// On failure output and written are unchanged; input/output must not overlap.
inline bool decodeBase64(std::string_view input, std::span<uint8_t> output, size_t &written) noexcept {
    size_t count = 0;
    if (!base64Size(input, count) || output.size() < count) return false;
    size_t cursor = 0;
    for (size_t i = 0; i < input.size(); i += 4) {
        const auto a = static_cast<unsigned>(base64Digit(input[i]));
        const auto b = static_cast<unsigned>(base64Digit(input[i + 1]));
        const auto c = input[i + 2] == '=' ? 0u : static_cast<unsigned>(base64Digit(input[i + 2]));
        const auto d = input[i + 3] == '=' ? 0u : static_cast<unsigned>(base64Digit(input[i + 3]));
        output[cursor++] = static_cast<uint8_t>((a << 2) | (b >> 4));
        if (cursor < count) output[cursor++] = static_cast<uint8_t>((b << 4) | (c >> 2));
        if (cursor < count) output[cursor++] = static_cast<uint8_t>((c << 6) | d);
    }
    written = count; return true;
}
}
