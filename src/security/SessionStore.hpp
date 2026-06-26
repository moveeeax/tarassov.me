/**
 * @file SessionStore.hpp
 * @brief Refresh-token revocation store (Redis), shared by the auth and
 *        account controllers.
 * @details Each issued refresh token has a JTI written to
 *          `<prefix><jti> = "1"` with the refresh TTL; /refresh checks it,
 *          logout deletes it. To support "log out everywhere" on a password
 *          change/reset we ALSO index every JTI in a per-user set
 *          `<prefix>user:<sub>` so revoke_all() can sweep them.
 *
 *          All ops are best-effort (Redis fail-open); callers that need the
 *          fail-closed signal (mint_session) check the bool return.
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "cache/Cache.hpp"
#include "security/Auth.hpp"

namespace Security::Sessions {

inline std::string jti_key(const Auth::CookieConfig& cfg, const std::string& jti) {
    return cfg.refresh_revocation_prefix + jti;
}

inline std::string user_set_key(const Auth::CookieConfig& cfg, const std::string& sub) {
    return cfg.refresh_revocation_prefix + "user:" + sub;
}

/**
 * @brief Record a freshly-issued refresh JTI as live + index it under the
 *        user. Returns false if the live-marker write failed (Redis down) —
 *        mint_session treats that as fail-closed.
 */
inline bool record(const Auth::CookieConfig& cfg, const std::string& sub, const std::string& jti, int ttl_sec) {
    if (!Cache::is_initialized())
        return false;
    if (!Cache::get().set(jti_key(cfg, jti), "1", ttl_sec))
        return false;
    // Index for revoke_all. Best-effort: a missed sadd only means this JTI
    // won't be swept by a future "log out everywhere" — the per-JTI marker
    // above (and its TTL) still governs validity.
    try {
        auto& redis = Cache::get().get_client();
        redis.sadd(user_set_key(cfg, sub), jti);
        // Keep the set from outliving the longest possible session.
        redis.expire(user_set_key(cfg, sub), ttl_sec);
    } catch (...) {}
    return true;
}

inline bool is_live(const Auth::CookieConfig& cfg, const std::string& jti) {
    try {
        if (!Cache::is_initialized())
            return false;
        return Cache::get().get(jti_key(cfg, jti)).has_value();
    } catch (...) {
        return false;
    }
}

inline void revoke_jti(const Auth::CookieConfig& cfg, const std::string& jti) {
    try {
        if (Cache::is_initialized())
            Cache::get().del(jti_key(cfg, jti));
    } catch (...) {}
}

/**
 * @brief Revoke every REFRESH session of a user — used on password
 *        reset/change. NB: access JWTs are stateless and are NOT re-checked
 *        against this store per request, so an already-issued access token stays
 *        valid until it expires (access TTL, ~15 min). This closes the refresh
 *        path (no new access tokens can be minted), not that residual window —
 *        don't describe it as "immediate lockout".
 */
inline void revoke_all(const Auth::CookieConfig& cfg, const std::string& sub) {
    if (!Cache::is_initialized())
        return;
    try {
        auto& redis = Cache::get().get_client();
        std::vector<std::string> jtis;
        redis.smembers(user_set_key(cfg, sub), std::back_inserter(jtis));
        for (const auto& jti : jtis) {
            try {
                redis.del(jti_key(cfg, jti));
            } catch (...) {}
        }
        redis.del(user_set_key(cfg, sub));
    } catch (...) {}
}

}  // namespace Security::Sessions
