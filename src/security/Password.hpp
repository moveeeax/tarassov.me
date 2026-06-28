/**
 * @file Password.hpp
 * @brief Password hashing — argon2id via libsodium.
 *
 * flask-base parity: app/models/user.py uses werkzeug's
 * generate_password_hash + check_password_hash (PBKDF2-SHA256 by default).
 * We pick argon2id instead — it's the OWASP recommendation for password
 * storage, libsodium ships a tested implementation, and the resulting
 * hash string is self-contained (`$argon2id$v=...$m=...$t=...$p=...$salt$hash`)
 * so we don't need a separate salt column.
 *
 * Single static-init guard runs `sodium_init()` exactly once across the
 * process. Calls before init are guarded by ensure_initialized().
 *
 * Public API:
 *   std::string hash    = Password::hash("plaintext");        // ≈ 90 ms
 *   bool        match   = Password::verify("plaintext", hash); // ≈ 90 ms
 *   bool        is_hash = Password::looks_hashed(hash);
 *
 * Cost is intentionally non-zero (~90 ms on modern hardware with the
 * INTERACTIVE preset) — that's the whole point of a password hash.
 */

#pragma once

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sodium.h>

namespace Security::Password {

/**
 * @brief Idempotent libsodium init. Throws on first-time failure.
 *        Subsequent calls are a no-op.
 */
inline void ensure_initialized() {
    static const int rc = ::sodium_init();  // returns 0 on first init, 1 if already initialized
    if (rc < 0)
        throw std::runtime_error("libsodium failed to initialize");
}

/**
 * @brief Argon2id hash using INTERACTIVE ops/mem limits.
 * @details INTERACTIVE is the libsodium-recommended preset for online
 *          login (~90 ms, 64 MiB RAM). For higher-value secrets bump to
 *          MODERATE or SENSITIVE — costs scale linearly.
 */
inline std::string hash(std::string_view plaintext) {
    ensure_initialized();
    if (plaintext.empty())
        throw std::invalid_argument("password must be non-empty");
    if (plaintext.size() > crypto_pwhash_PASSWD_MAX)
        throw std::invalid_argument("password too long");

    char out[crypto_pwhash_STRBYTES];
    if (::crypto_pwhash_str(out,
                            plaintext.data(),
                            plaintext.size(),
                            crypto_pwhash_OPSLIMIT_INTERACTIVE,
                            crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("argon2 hashing failed (out of memory?)");
    }
    return std::string(out);
}

/**
 * @brief Constant-time verify. Returns false (not throws) on malformed
 *        stored hashes — callers should treat that the same as wrong
 *        password to avoid leaking which accounts have hashes vs. don't.
 */
inline bool verify(std::string_view plaintext, std::string_view stored_hash) {
    ensure_initialized();
    if (plaintext.empty() || stored_hash.empty())
        return false;
    if (stored_hash.size() >= crypto_pwhash_STRBYTES)
        return false;  // would not fit in libsodium's expected buffer

    char buf[crypto_pwhash_STRBYTES] = {};
    std::memcpy(buf, stored_hash.data(), stored_hash.size());

    return ::crypto_pwhash_str_verify(buf, plaintext.data(), plaintext.size()) == 0;
}

/**
 * @brief Cheap shape check — useful when migrating legacy hashes or
 *        deciding whether a column already holds an argon2 string.
 *        Real verification is verify(); this is just a prefix sniff.
 */
inline bool looks_hashed(std::string_view s) {
    return s.starts_with("$argon2id$") || s.starts_with("$argon2i$") || s.starts_with("$argon2d$");
}

}  // namespace Security::Password
