/**
 * @file Auth.hpp
 * @brief Authentication / authorization module.
 * @details Supports three modes:
 *          - "none"   — no auth (default, for local dev)
 *          - "bearer" — static Bearer token (legacy, for quick internal tests)
 *          - "jwt"    — HS256 JWT: signature + exp/nbf/iss/aud validation,
 *                       role-based authorization via claim (default "roles").
 *          The middleware is installed by Api::register_controllers(); RBAC
 *          helpers (require_role, has_role, principal_of) are available to
 *          any controller via Security::.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "security/Jwt.hpp"
#include "security/SessionCookies.hpp"
#include "utils/Config.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Strings.hpp"
#include "utils/Time.hpp"

namespace Security::Auth {

using json = nlohmann::json;

enum class AuthMode { None, Bearer, Jwt };

// Permission bitmask that denotes a full administrator: a DEDICATED sentinel
// bit, NOT 0xff "all bits" (with 0xff a role accumulating the eight low feature
// bits would accidentally become admin). Mirrors Domain::Permission::kAdminister
// — duplicated here (rather than including Domain) to keep the Auth layer below
// the Domain layer. MUST stay in sync; tests/unit/test_auth_permissions.cpp
// asserts equality so the two can't drift.
inline constexpr std::uint32_t kAdminPermissionBits = 0x40000000u;

struct AuthConfig {
    AuthMode mode = AuthMode::None;

    // Bearer mode.
    std::string bearer_token;

    // JWT HS256. Single shared secret — for kid-based key rotation, swap in
    // an external KMS or extend Authenticator with a JWKS resolver.
    std::string jwt_secret;
    std::string jwt_issuer;
    std::string jwt_audience;
    int jwt_leeway_sec = 30;
    std::string jwt_roles_claim = "roles";
    std::string jwt_scopes_claim = "scope";

    // Paths that never require auth. Exact-match only.
    std::unordered_set<std::string> public_paths;

    // Cookie-mode settings. Off by default; turn on for SPA flows.
    CookieConfig cookies;
};

struct AuthPrincipal {
    std::string subject;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
    json raw_claims;
};

namespace detail {

inline std::vector<std::string> extract_string_array(const json& node) {
    std::vector<std::string> out;
    if (node.is_array()) {
        for (const auto& v : node) {
            if (v.is_string())
                out.push_back(v.get<std::string>());
        }
    } else if (node.is_string()) {
        // Space-separated (OAuth2 scope convention).
        std::istringstream iss(node.get<std::string>());
        std::string tok;
        while (iss >> tok)
            out.push_back(tok);
    }
    return out;
}

}  // namespace detail

/**
 * @brief Authenticator: owns AuthConfig and verifies incoming tokens.
 */
class Authenticator {
public:
    explicit Authenticator(AuthConfig cfg) : config_(std::move(cfg)) {}

    const AuthConfig& config() const { return config_; }

    /**
     * @brief Verify a JWT Bearer token; populate principal on success.
     * @return empty optional + err code on failure.
     */
    std::optional<AuthPrincipal> verify_jwt(const std::string& token, std::string& err) const {
        auto claims_opt = verify_hs256_jwt(token, config_.jwt_secret, err);
        if (!claims_opt)
            return std::nullopt;
        const auto& claims = *claims_opt;

        // A refresh token must never authenticate a request. It outlives the
        // access TTL (7d vs 15m) and is only revocation-checked on /refresh,
        // so accepting it here would bypass both. We reject typ=="refresh"
        // rather than require typ=="access" so typ-less third-party access
        // tokens (and make-jwt.sh) keep working.
        if (claims.value("typ", "") == "refresh") {
            err = "wrong_token_type";
            return std::nullopt;
        }

        const auto now = Utils::Time::now_epoch_seconds();
        const auto leeway = static_cast<int64_t>(config_.jwt_leeway_sec);

        if (claims.contains("exp") && claims["exp"].is_number_integer()) {
            auto exp = claims["exp"].get<int64_t>();
            if (now > exp + leeway) {
                err = "token_expired";
                return std::nullopt;
            }
        }
        if (claims.contains("nbf") && claims["nbf"].is_number_integer()) {
            auto nbf = claims["nbf"].get<int64_t>();
            if (now + leeway < nbf) {
                err = "token_not_yet_valid";
                return std::nullopt;
            }
        }
        if (!config_.jwt_issuer.empty() && claims.value("iss", "") != config_.jwt_issuer) {
            err = "bad_issuer";
            return std::nullopt;
        }
        if (!config_.jwt_audience.empty()) {
            bool aud_ok = false;
            if (claims.contains("aud")) {
                if (claims["aud"].is_string()) {
                    aud_ok = (claims["aud"].get<std::string>() == config_.jwt_audience);
                } else if (claims["aud"].is_array()) {
                    for (const auto& a : claims["aud"]) {
                        if (a.is_string() && a.get<std::string>() == config_.jwt_audience) {
                            aud_ok = true;
                            break;
                        }
                    }
                }
            }
            if (!aud_ok) {
                err = "bad_audience";
                return std::nullopt;
            }
        }

        AuthPrincipal p;
        p.subject = claims.value("sub", "");
        if (claims.contains(config_.jwt_roles_claim)) {
            p.roles = detail::extract_string_array(claims[config_.jwt_roles_claim]);
        }
        if (claims.contains(config_.jwt_scopes_claim)) {
            p.scopes = detail::extract_string_array(claims[config_.jwt_scopes_claim]);
        }
        p.raw_claims = claims;
        return p;
    }

    bool path_is_public(const std::string& path) const {
        return Utils::Strings::path_is_public(config_.public_paths, path);
    }

private:
    AuthConfig config_;
};

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------

inline std::unique_ptr<Authenticator> global_auth = nullptr;

inline AuthMode parse_mode(const std::string& s) {
    if (s == "jwt")
        return AuthMode::Jwt;
    if (s == "bearer")
        return AuthMode::Bearer;
    return AuthMode::None;
}

/**
 * @brief Build an AuthConfig from the process Config singleton.
 * @throws std::runtime_error if the mode requires a secret that is not set.
 */
inline AuthConfig load_config_from_global() {
    AuthConfig cfg;
    if (!Config::is_initialized())
        return cfg;
    auto& c = Config::get();

    std::string mode_str = c.get<std::string>("auth.mode", "AUTH_MODE", "none");
    cfg.mode = parse_mode(mode_str);

    cfg.bearer_token = c.get<std::string>("auth.bearer_token", "AUTH_BEARER_TOKEN", "");
    cfg.jwt_secret = c.get<std::string>("auth.jwt.secret", "JWT_SECRET", "");
    cfg.jwt_issuer = c.get<std::string>("auth.jwt.issuer", "JWT_ISSUER", "");
    cfg.jwt_audience = c.get<std::string>("auth.jwt.audience", "JWT_AUDIENCE", "");
    cfg.jwt_leeway_sec = c.get<int>("auth.jwt.leeway_sec", "JWT_LEEWAY_SEC", 30);
    cfg.jwt_roles_claim = c.get<std::string>("auth.jwt.roles_claim", "JWT_ROLES_CLAIM", "roles");
    cfg.jwt_scopes_claim = c.get<std::string>("auth.jwt.scopes_claim", "JWT_SCOPES_CLAIM", "scope");

    // Single source of truth for public paths across Auth + RateLimit +
    // Idempotency. Module-specific override keys intentionally NOT supported
    // to prevent drift between middlewares.
    std::string paths_csv =
        c.get<std::string>("api.public_paths", "API_PUBLIC_PATHS", Utils::Strings::kDefaultPublicPathsCsv);
    cfg.public_paths = Utils::Strings::split_csv_set(paths_csv);

    if (cfg.mode == AuthMode::Jwt && cfg.jwt_secret.empty()) {
        throw std::runtime_error("auth.mode=jwt but JWT_SECRET is empty — set auth.jwt.secret or JWT_SECRET env");
    }
    // Enforce a minimum secret length at boot (not just non-empty): a short
    // HS256 key is brute-forceable offline → token forgery → full admin. The
    // same master secret derives the email-link token keys (Tokens.hpp), so this
    // guards both surfaces. 32 bytes matches the HMAC-SHA256 output size and the
    // prod-check.sh threshold (which is opt-in and didn't cover this path).
    if (cfg.mode == AuthMode::Jwt && cfg.jwt_secret.size() < 32) {
        throw std::runtime_error("auth.mode=jwt but JWT_SECRET is shorter than 32 chars — use a strong random secret");
    }
    if (cfg.mode == AuthMode::Bearer && cfg.bearer_token.empty()) {
        throw std::runtime_error("auth.mode=bearer but AUTH_BEARER_TOKEN is empty");
    }

    // Cookie session config — Config::get already resolves ${VAR}
    // placeholders during load_from_file → substitute_env_placeholders, so
    // the canonical layered lookup (env > json default > built-in default)
    // works for cookie fields too.
    cfg.cookies.enabled = c.get<bool>("auth.cookies.enabled", "AUTH_COOKIES_ENABLED", false);
    cfg.cookies.access_name = c.get<std::string>("auth.cookies.access_name", "AUTH_COOKIE_ACCESS", "__Host-access");
    cfg.cookies.refresh_name = c.get<std::string>("auth.cookies.refresh_name", "AUTH_COOKIE_REFRESH", "__Host-refresh");
    cfg.cookies.access_ttl_sec = c.get<int>("auth.cookies.access_ttl_sec", "AUTH_COOKIE_ACCESS_TTL_SEC", 15 * 60);
    cfg.cookies.refresh_ttl_sec =
        c.get<int>("auth.cookies.refresh_ttl_sec", "AUTH_COOKIE_REFRESH_TTL_SEC", 7 * 24 * 60 * 60);
    cfg.cookies.secure = c.get<bool>("auth.cookies.secure", "AUTH_COOKIE_SECURE", true);
    cfg.cookies.samesite = c.get<std::string>("auth.cookies.samesite", "AUTH_COOKIE_SAMESITE", "Lax");
    cfg.cookies.refresh_revocation_prefix =
        c.get<std::string>("auth.cookies.refresh_revocation_prefix", "AUTH_COOKIE_REVOCATION_PREFIX", "auth:refresh:");
    // CSRF lives under security.csrf.* but rides on the cookie config so
    // set_session_cookies can emit the token cookie. Off by default.
    cfg.cookies.csrf_enabled = c.get<bool>("security.csrf.enabled", "SECURITY_CSRF_ENABLED", false);
    cfg.cookies.csrf_cookie_name =
        c.get<std::string>("security.csrf.cookie_name", "SECURITY_CSRF_COOKIE", "csrf-token");
    spdlog::info("Auth cookies: enabled={} access_name={} samesite={} secure={}",
                 cfg.cookies.enabled,
                 cfg.cookies.access_name,
                 cfg.cookies.samesite,
                 cfg.cookies.secure);
    return cfg;
}

inline void initialize() {
    if (global_auth != nullptr) {
        // Same convention as RateLimit/Idempotency: repeated initialize is a
        // warned no-op (Core may be re-run in tests); use shutdown() first
        // to reconfigure.
        spdlog::warn("Auth::initialize called twice — keeping existing config");
        return;
    }
    auto cfg = load_config_from_global();
    global_auth = std::make_unique<Authenticator>(std::move(cfg));
    const char* mode_name = "none";
    switch (global_auth->config().mode) {
        case AuthMode::Bearer:
            mode_name = "bearer";
            break;
        case AuthMode::Jwt:
            mode_name = "jwt";
            break;
        case AuthMode::None:
            mode_name = "none";
            break;
    }
    spdlog::info(
        "Auth module initialized: mode={} public_paths={}", mode_name, global_auth->config().public_paths.size());
}

inline bool is_initialized() {
    return global_auth != nullptr;
}

inline Authenticator& get() {
    if (global_auth == nullptr) {
        throw std::runtime_error("Auth not initialized");
    }
    return *global_auth;
}

inline void shutdown() {
    global_auth.reset();
}

// Test seam: install a global Authenticator from an explicit config, bypassing
// Config/env — so guards like require_confirmed are unit-testable in a chosen
// mode. Mirrors Cache::install_for_testing. Pair with shutdown() in TearDown.
inline void install_for_testing(AuthConfig cfg) {
    global_auth = std::make_unique<Authenticator>(std::move(cfg));
}

// ---------------------------------------------------------------------------
// RBAC helpers for controllers
// ---------------------------------------------------------------------------

inline constexpr const char* kPrincipalAttr = "_auth_principal";

inline std::optional<AuthPrincipal> principal_of(const drogon::HttpRequestPtr& req) {
    // Drogon's Attributes::get<T>() behaviour on a missing key differs by
    // version (throw std::out_of_range vs. return a default-constructed T) —
    // check presence explicitly so an absent principal can never masquerade
    // as an empty-subject one (which downstream code would feed to SQL).
    if (!req->attributes()->find(kPrincipalAttr))
        return std::nullopt;
    try {
        return req->attributes()->get<AuthPrincipal>(kPrincipalAttr);
    } catch (...) {
        return std::nullopt;
    }
}

inline bool has_role(const drogon::HttpRequestPtr& req, const std::string& role) {
    auto p = principal_of(req);
    if (!p)
        return false;
    return std::find(p->roles.begin(), p->roles.end(), role) != p->roles.end();
}

inline bool has_any_role(const drogon::HttpRequestPtr& req, const std::vector<std::string>& roles) {
    auto p = principal_of(req);
    if (!p)
        return false;
    for (const auto& want : roles) {
        if (std::find(p->roles.begin(), p->roles.end(), want) != p->roles.end())
            return true;
    }
    return false;
}

/**
 * @brief Returns a 403 response if the request's principal lacks the role,
 *        or nullptr if it has the role (or auth is disabled).
 */
inline drogon::HttpResponsePtr require_role(const drogon::HttpRequestPtr& req, const std::string& role) {
    if (!is_initialized() || get().config().mode == AuthMode::None)
        return {};
    if (has_role(req, role))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden, "forbidden", "", nlohmann::json{{"required_role", role}}});
}

inline drogon::HttpResponsePtr require_any_role(const drogon::HttpRequestPtr& req,
                                                const std::vector<std::string>& roles) {
    if (!is_initialized() || get().config().mode == AuthMode::None)
        return {};
    if (has_any_role(req, roles))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden, "forbidden", "", nlohmann::json{{"required_roles", roles}}});
}

/**
 * @brief nullptr if the caller's email is confirmed (or auth is disabled);
 *        401 if anonymous; 403 if authenticated but unconfirmed. The "confirmed"
 *        boolean is read from the access JWT claim (minted at login), so no DB
 *        hit. flask-base parity: @confirmed_required. The flag is loaded
 *        everywhere but not enforced by default — gate your domain's
 *        confirmation-required routes with API_REQUIRE_CONFIRMED.
 */
inline drogon::HttpResponsePtr require_confirmed(const drogon::HttpRequestPtr& req) {
    if (!is_initialized() || get().config().mode == AuthMode::None)
        return {};
    auto p = principal_of(req);
    if (!p)
        return ErrorResponse::unauthorized();
    if (p->raw_claims.value("confirmed", false))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden,
                                "email_unconfirmed",
                                "Confirm your email address to access this resource",
                                nlohmann::json{}});
}

// ---------------------------------------------------------------------------
// Permission bitmask helpers — flask-base parity: app/decorators.py
// permission_required / admin_required.
//
// The access JWT carries a "permissions" int claim whose bits match the
// constants in src/domain/Role.hpp (Permission::kGeneral, kAdminister, ...).
// Reading from the claim avoids a DB hit on every guarded request.
// ---------------------------------------------------------------------------

/**
 * @brief Bitmask of the current principal's permissions, or 0 if there
 *        isn't one. Read from the "permissions" claim of the access JWT.
 */
inline std::uint32_t current_permissions(const drogon::HttpRequestPtr& req) {
    auto p = principal_of(req);
    if (!p)
        return 0;
    if (!p->raw_claims.contains("permissions"))
        return 0;
    const auto& v = p->raw_claims["permissions"];
    if (!v.is_number_integer())
        return 0;
    // get<int64_t>, not long: long is 32-bit on Windows, where a uint32 bitmask
    // with the high bit set would overflow on the way through.
    return static_cast<std::uint32_t>(v.get<std::int64_t>());
}

inline bool current_user_can(const drogon::HttpRequestPtr& req, std::uint32_t perm) {
    const std::uint32_t have = current_permissions(req);
    // The admin sentinel bit satisfies EVERY permission check — admin can do
    // anything. This was implicit when admin was 0xff (all low feature bits);
    // with the dedicated sentinel bit it must be explicit, or admins would be
    // rejected by feature-permission gates (e.g. Permission::kAuditRead).
    if ((have & kAdminPermissionBits) == kAdminPermissionBits)
        return true;
    return (have & perm) == perm;
}

inline bool current_user_is_admin(const drogon::HttpRequestPtr& req) {
    return current_user_can(req, kAdminPermissionBits);
}

/**
 * @brief Returns a 403 response if the request's principal lacks the
 *        permission, or nullptr if it has it (or auth is disabled).
 */
inline drogon::HttpResponsePtr require_permission(const drogon::HttpRequestPtr& req, std::uint32_t perm) {
    if (!is_initialized() || get().config().mode == AuthMode::None)
        return {};
    if (current_user_can(req, perm))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden, "forbidden", "", nlohmann::json{{"required_permission", perm}}});
}

inline drogon::HttpResponsePtr require_admin(const drogon::HttpRequestPtr& req) {
    return require_permission(req, kAdminPermissionBits);
}

}  // namespace Security::Auth
