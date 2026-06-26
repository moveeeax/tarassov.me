/**
 * @file Base64.hpp
 * @brief base64url encode / decode (RFC 4648 §5, no padding).
 *
 * Used by Auth (JWT), Tokens (signed timed link tokens), and any future
 * primitive that needs to round-trip bytes through a URL-safe channel.
 * Centralized here so multiple modules don't carry near-identical copies.
 */

#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Utils::Base64 {

inline std::string url_encode(std::string_view in) {
    static constexpr const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() * 4) / 3 + 4);
    int bits = 0;
    std::uint32_t buf = 0;
    for (unsigned char c : in) {
        buf = (buf << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alpha[(buf >> bits) & 0x3F]);
        }
    }
    if (bits > 0)
        out.push_back(alpha[(buf << (6 - bits)) & 0x3F]);
    return out;
}

inline std::string url_decode(std::string_view in) {
    static const auto table = [] {
        std::array<std::int8_t, 256> t{};
        t.fill(-1);
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(alpha[i])] = static_cast<std::int8_t>(i);
        return t;
    }();

    std::string out;
    out.reserve((in.size() * 3) / 4 + 2);
    std::uint32_t buf = 0;
    int bits = 0;
    std::size_t ndigits = 0;  // significant (non-padding/whitespace) chars
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        int v = table[static_cast<unsigned char>(c)];
        if (v < 0)
            throw std::runtime_error("invalid base64url");
        ++ndigits;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    // Reject non-canonical encodings that decode to the same bytes as a valid
    // token — otherwise a different token *string* round-trips to an identical
    // HMAC/signature and slips past one-shot/JTI replay guards (token
    // malleability). A length of 4k+1 chars is structurally impossible, and the
    // trailing `bits` partial group must be zero-padded by the encoder.
    if (ndigits % 4 == 1)
        throw std::runtime_error("invalid base64url length");
    if (bits > 0 && (buf & ((1U << bits) - 1)) != 0)
        throw std::runtime_error("invalid base64url padding bits");
    return out;
}

}  // namespace Utils::Base64
