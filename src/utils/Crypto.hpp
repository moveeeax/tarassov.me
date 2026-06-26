/**
 * @file Crypto.hpp
 * @brief HMAC-SHA256 + constant-time compare + random hex.
 *
 * Centralized so Auth (JWT), Tokens (link tokens) and any future primitive
 * share one implementation instead of carrying near-identical copies.
 */

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace Utils::Crypto {

namespace detail {
/// Lowercase hex encoding of @p n bytes. Single source for random_hex/sha256_hex.
inline std::string bytes_to_hex(const unsigned char* data, std::size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}
}  // namespace detail

inline std::string hmac_sha256(std::string_view key, std::string_view data) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    const unsigned char* out = HMAC(EVP_sha256(),
                                    key.data(),
                                    static_cast<int>(key.size()),
                                    reinterpret_cast<const unsigned char*>(data.data()),
                                    data.size(),
                                    mac,
                                    &mac_len);
    if (out == nullptr)
        throw std::runtime_error("HMAC-SHA256 failed");
    return std::string(reinterpret_cast<char*>(mac), mac_len);
}

inline bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

/**
 * @brief Random hex string of @p byte_count bytes (so output is byte_count*2 chars).
 *        Fails closed: throws if the CSPRNG is unavailable rather than degrading
 *        to a guessable clock value (this feeds refresh-token JTIs / link tokens,
 *        where a predictable value would weaken the revocation namespace).
 */
inline std::string random_hex(std::size_t byte_count) {
    unsigned char buf[64];
    if (byte_count > sizeof(buf))
        byte_count = sizeof(buf);
    if (RAND_bytes(buf, static_cast<int>(byte_count)) != 1) {
        throw std::runtime_error("CSPRNG (RAND_bytes) failed");
    }
    return detail::bytes_to_hex(buf, byte_count);
}

/**
 * @brief Lowercase hex SHA-256 of @p s. Used by the Idempotency middleware
 *        to fingerprint request/response bodies.
 */
inline std::string sha256_hex(std::string_view s) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr)
        throw std::runtime_error("EVP_MD_CTX_new failed");
    // Check every OpenSSL return — on failure `hash` would be uninitialized
    // and we'd hand back a garbage digest as if it were valid (this feeds the
    // idempotency fingerprint).
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 || EVP_DigestUpdate(ctx, s.data(), s.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP SHA-256 digest failed");
    }
    EVP_MD_CTX_free(ctx);
    return detail::bytes_to_hex(hash, len);
}

}  // namespace Utils::Crypto
