/**
 * @file test_fixtures.hpp
 * @brief Reusable test scenarios — JWTs, principals, jobs, common setups.
 *
 * test_helpers.hpp already covers infrastructure plumbing (config, reset,
 * connectivity checks). This file adds *domain* fixtures that several test
 * files would otherwise build by hand.
 *
 * Keep fixtures small and explicit: every helper has a clear "what scenario
 * does this represent." Avoid building a 'god fixture' that pulls in every
 * subsystem — pick the smallest set the test actually needs.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "jobs/Jobs.hpp"
#include "security/Auth.hpp"
#include "utils/Base64.hpp"
#include "utils/Crypto.hpp"

namespace TestFixtures {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// JWT helpers — call these to mint tokens for security/auth tests without
// duplicating the base64url + HMAC dance in every file.
// ---------------------------------------------------------------------------

inline int64_t now_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string hmac256_raw(const std::string& key, const std::string& data) {
    return Utils::Crypto::hmac_sha256(key, data);
}

/// Build a compact-serialized HS256 JWT from claims.
inline std::string make_jwt(const json& claims, const std::string& secret) {
    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    std::string h = Utils::Base64::url_encode(header.dump());
    std::string p = Utils::Base64::url_encode(claims.dump());
    std::string signing = h + "." + p;
    std::string sig = Utils::Base64::url_encode(Utils::Crypto::hmac_sha256(secret, signing));
    return signing + "." + sig;
}

/// Standard "happy path" claim set: subject, issuer, audience, exp 1h ahead.
inline json default_claims(const std::string& subject = "user-1",
                           const std::string& issuer = "test-iss",
                           const std::string& audience = "test-aud") {
    return json{
        {"sub", subject},
        {"iss", issuer},
        {"aud", audience},
        {"iat", now_sec()},
        {"exp", now_sec() + 3600},
    };
}

inline std::string admin_jwt(const std::string& secret = "test-secret") {
    json claims = default_claims("admin-user");
    claims["roles"] = json::array({"admin", "user"});
    return make_jwt(claims, secret);
}

inline std::string user_jwt(const std::string& secret = "test-secret") {
    json claims = default_claims("regular-user");
    claims["roles"] = json::array({"user"});
    return make_jwt(claims, secret);
}

inline std::string expired_jwt(const std::string& secret = "test-secret") {
    json claims = default_claims();
    claims["exp"] = now_sec() - 60;  // 1 min in the past
    claims["iat"] = now_sec() - 3660;
    return make_jwt(claims, secret);
}

inline std::string future_jwt(const std::string& secret = "test-secret") {
    json claims = default_claims();
    claims["nbf"] = now_sec() + 300;  // not valid for 5 min
    claims["iat"] = now_sec() + 300;
    return make_jwt(claims, secret);
}

// ---------------------------------------------------------------------------
// Auth principal builders — for testing controllers or middleware that read
// `req->attributes()->get<AuthPrincipal>(kPrincipalAttr)`.
// ---------------------------------------------------------------------------

inline Security::Auth::AuthPrincipal admin_principal(const std::string& sub = "admin-user") {
    Security::Auth::AuthPrincipal p;
    p.subject = sub;
    p.roles = {"admin", "user"};
    p.scopes = {};
    p.raw_claims = json::object();
    return p;
}

inline Security::Auth::AuthPrincipal user_principal(const std::string& sub = "regular-user") {
    Security::Auth::AuthPrincipal p;
    p.subject = sub;
    p.roles = {"user"};
    p.scopes = {};
    p.raw_claims = json::object();
    return p;
}

// ---------------------------------------------------------------------------
// Job fixtures — return Job structs in a known state. Useful for unit tests
// of Jobs::Manager / DLQ flows that don't need to round-trip through Redis.
// ---------------------------------------------------------------------------

inline Jobs::Job pending_job(const std::string& type = "echo", const json& payload = json::object()) {
    Jobs::Job j;
    j.id = "00000000-0000-0000-0000-000000000001";
    j.type = type;
    j.payload = payload;
    j.status = "pending";
    j.created_at = now_sec();
    return j;
}

inline Jobs::Job running_job(const std::string& type = "slow") {
    auto j = pending_job(type);
    j.id = "00000000-0000-0000-0000-000000000002";
    j.status = "processing";
    j.worker_id = "worker-test";
    return j;
}

inline Jobs::Job failed_job(const std::string& type = "fail", const std::string& error = "boom") {
    auto j = pending_job(type);
    j.id = "00000000-0000-0000-0000-000000000003";
    j.status = "failed";
    j.error = error;
    j.retry_count = 3;
    return j;
}

}  // namespace TestFixtures
