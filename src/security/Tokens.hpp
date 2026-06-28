/**
 * @file Tokens.hpp
 * @brief HMAC-signed timed single-purpose tokens for email links.
 *
 * flask-base parity: app/models/user.py — User.generate_*_token /
 * confirm_account / change_email / reset_password use itsdangerous's
 * URLSafeTimedSerializer (HMAC + timed payload). We reproduce the same
 * primitive in C++:
 *
 *   payload = JSON {"sub": "<uuid>", "purpose": "<confirm|reset|change_email>",
 *                   "exp": <unix>, ...optional...}
 *   sig     = HMAC-SHA256(secret_for(purpose), base64url(payload))
 *   token   = base64url(payload) "." base64url(sig)
 *
 * NOT a JWT — JWTs are for session credentials. These are short-lived,
 * single-purpose link tokens with no header field. Keeping the format
 * minimal also keeps URLs short enough for emails that wrap at 76 cols.
 *
 * Per-purpose key derivation: HMAC-SHA256(JWT_SECRET, "tokens:<purpose>")
 * so a leaked confirm-token-key can't sign reset-password tokens. Caller
 * provides the master secret on construction.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "utils/Base64.hpp"
#include "utils/Crypto.hpp"
#include "utils/Time.hpp"

namespace Security::Tokens {

using json = nlohmann::json;

enum class Purpose {
    Confirm,        // initial email confirmation
    ResetPassword,  // forgot-password link
    ChangeEmail,    // confirm new email when changing
    Invite,         // admin-invited new user (matches flask-base /join-from-invite)
};

inline std::string purpose_string(Purpose p) {
    switch (p) {
        case Purpose::Confirm:
            return "confirm";
        case Purpose::ResetPassword:
            return "reset";
        case Purpose::ChangeEmail:
            return "change_email";
        case Purpose::Invite:
            return "invite";
    }
    return "unknown";
}

namespace detail {

/**
 * @brief Derive a per-purpose key from a master secret. So a leak in the
 *        password-reset code path can't be used to forge confirm tokens.
 */
inline std::string derive_key(std::string_view master, Purpose p) {
    return Utils::Crypto::hmac_sha256(master, "tokens:" + purpose_string(p));
}

}  // namespace detail

/**
 * @brief Issue a single-purpose signed link token.
 * @param master_secret  Same secret as JWT_SECRET — separate key derivation
 *                       below means a confirm-token leak can't sign resets.
 * @param sub            Stable user identifier (usually the user UUID).
 * @param purpose        What this token is for. Verifier MUST check it.
 * @param ttl            Lifetime. flask-base defaults: confirm = 7d,
 *                       reset = 1h, change_email = 1h, invite = 7d.
 * @param extra          Optional extra claims merged into payload (e.g.
 *                       {"new_email": "..."} for change-email tokens).
 */
inline std::string issue(std::string_view master_secret,
                         std::string_view sub,
                         Purpose purpose,
                         std::chrono::seconds ttl,
                         const json& extra = json::object()) {
    if (master_secret.empty())
        throw std::invalid_argument("master_secret must be set");

    const auto now_s = Utils::Time::now_epoch_seconds();

    json payload = {
        {"sub", std::string(sub)},
        {"purpose", purpose_string(purpose)},
        {"iat", now_s},
        {"exp", now_s + static_cast<std::int64_t>(ttl.count())},
    };
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it)
            payload[it.key()] = it.value();
    }

    const std::string payload_b64 = Utils::Base64::url_encode(payload.dump());
    const std::string key = detail::derive_key(master_secret, purpose);
    const std::string sig_b64 = Utils::Base64::url_encode(Utils::Crypto::hmac_sha256(key, payload_b64));
    return payload_b64 + "." + sig_b64;
}

struct VerifyResult {
    bool ok = false;
    std::string error;            // human reason if !ok ("expired", "invalid_signature", ...)
    std::string sub;              // populated if ok
    json extra = json::object();  // claims beyond sub/purpose/iat/exp
};

/**
 * @brief Verify and parse a token. Returns VerifyResult.ok=false with an
 *        error code on every failure mode (malformed / bad-sig / expired /
 *        wrong-purpose) — controllers map these to a single 400 to defeat
 *        token-shape probing.
 */
inline VerifyResult verify(std::string_view master_secret, std::string_view token, Purpose expected_purpose) {
    VerifyResult vr;
    if (master_secret.empty()) {
        vr.error = "no_secret";
        return vr;
    }

    auto dot = token.find('.');
    if (dot == std::string_view::npos) {
        vr.error = "malformed";
        return vr;
    }
    std::string_view payload_b64 = token.substr(0, dot);
    std::string_view sig_b64 = token.substr(dot + 1);

    std::string actual_sig;
    std::string payload_raw;
    try {
        actual_sig = Utils::Base64::url_decode(sig_b64);
        payload_raw = Utils::Base64::url_decode(payload_b64);
    } catch (...) {
        vr.error = "malformed";
        return vr;
    }

    const std::string key = detail::derive_key(master_secret, expected_purpose);
    const std::string expected_sig = Utils::Crypto::hmac_sha256(key, payload_b64);
    if (!Utils::Crypto::constant_time_equals(expected_sig, actual_sig)) {
        vr.error = "invalid_signature";
        return vr;
    }

    json claims;
    try {
        claims = json::parse(payload_raw);
    } catch (...) {
        vr.error = "malformed";
        return vr;
    }

    auto purpose_it = claims.find("purpose");
    if (purpose_it == claims.end() || !purpose_it->is_string() ||
        purpose_it->get<std::string>() != purpose_string(expected_purpose)) {
        vr.error = "wrong_purpose";
        return vr;
    }

    auto exp_it = claims.find("exp");
    if (exp_it == claims.end() || !exp_it->is_number_integer()) {
        vr.error = "no_exp";
        return vr;
    }
    const auto now_s = Utils::Time::now_epoch_seconds();
    if (exp_it->get<std::int64_t>() < now_s) {
        vr.error = "expired";
        return vr;
    }

    auto sub_it = claims.find("sub");
    if (sub_it == claims.end() || !sub_it->is_string()) {
        vr.error = "no_sub";
        return vr;
    }
    vr.sub = sub_it->get<std::string>();

    // Extras: everything other than the canonical fields. Useful for
    // change-email tokens carrying the requested new_email.
    static const std::array<const char*, 4> reserved{"sub", "purpose", "iat", "exp"};
    for (auto it = claims.begin(); it != claims.end(); ++it) {
        bool is_reserved = false;
        for (auto r : reserved)
            if (it.key() == r) {
                is_reserved = true;
                break;
            }
        if (!is_reserved)
            vr.extra[it.key()] = it.value();
    }

    vr.ok = true;
    return vr;
}

}  // namespace Security::Tokens
