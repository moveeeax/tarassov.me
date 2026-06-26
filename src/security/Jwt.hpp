/**
 * @file Jwt.hpp
 * @brief HS256 JWT primitives: sign + verify-signature.
 * @details Claim validation (exp/nbf/iss/aud) lives in
 *          Security::Auth::Authenticator — these helpers only check shape
 *          and signature, so the refresh flow can reuse them for tokens
 *          with non-access claim sets.
 */

#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "utils/Base64.hpp"
#include "utils/Crypto.hpp"

namespace Security::Auth {

using json = nlohmann::json;

/**
 * @brief Verify an HS256 JWT and return the parsed claims on success.
 */
inline std::optional<json> verify_hs256_jwt(const std::string& token, const std::string& secret, std::string& err) {
    auto dot1 = token.find('.');
    auto dot2 = (dot1 == std::string::npos) ? std::string::npos : token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) {
        err = "malformed_token";
        return std::nullopt;
    }
    std::string header_b64 = token.substr(0, dot1);
    std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string sig_b64 = token.substr(dot2 + 1);

    json header;
    try {
        header = json::parse(Utils::Base64::url_decode(header_b64));
    } catch (...) {
        err = "malformed_header";
        return std::nullopt;
    }
    if (header.value("alg", "") != "HS256" || header.value("typ", "JWT") != "JWT") {
        err = "unsupported_alg";
        return std::nullopt;
    }
    if (secret.empty()) {
        err = "no_secret";
        return std::nullopt;
    }

    std::string signing_input = header_b64 + "." + payload_b64;
    std::string expected_sig = Utils::Crypto::hmac_sha256(secret, signing_input);
    std::string actual_sig;
    try {
        actual_sig = Utils::Base64::url_decode(sig_b64);
    } catch (...) {
        err = "malformed_signature";
        return std::nullopt;
    }
    if (!Utils::Crypto::constant_time_equals(expected_sig, actual_sig)) {
        err = "bad_signature";
        return std::nullopt;
    }

    json claims;
    try {
        claims = json::parse(Utils::Base64::url_decode(payload_b64));
    } catch (...) {
        err = "malformed_payload";
        return std::nullopt;
    }
    return claims;
}

/**
 * @brief Sign a fresh HS256 JWT.
 * @param claims  payload — caller is responsible for populating sub/exp/etc.
 *                We don't auto-add timestamps so issuer can pick its own
 *                (e.g. nbf for backdated tokens, iat for audit).
 * @param secret  HMAC key
 */
inline std::string issue_hs256_jwt(const json& claims, const std::string& secret) {
    if (secret.empty())
        throw std::invalid_argument("secret must be non-empty");
    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    const std::string header_b64 = Utils::Base64::url_encode(header.dump());
    const std::string payload_b64 = Utils::Base64::url_encode(claims.dump());
    const std::string signing_input = header_b64 + "." + payload_b64;
    const std::string sig = Utils::Crypto::hmac_sha256(secret, signing_input);
    return signing_input + "." + Utils::Base64::url_encode(sig);
}

}  // namespace Security::Auth
