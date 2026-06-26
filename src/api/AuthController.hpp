/**
 * @file AuthController.hpp
 * @brief Auth endpoints: register, login, logout, refresh, me.
 *
 * flask-base parity: app/account/views.py login() / register() / logout().
 * Differences from flask-base:
 *   - Returns JSON, not HTML. The frontend handles redirects.
 *   - Cookie-based session (HttpOnly + SameSite=Lax) instead of Flask-Login.
 *   - Refresh-token rotation: every /refresh returns a brand-new refresh JWT
 *     and invalidates the previous JTI in Redis. Logout deletes the JTI.
 *   - Email-confirmation token generation lives here so /register can fire
 *     it; the actual SMTP send is wired in stage 2 (AccountController +
 *     Mailer). Until then we log the link at INFO level.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "cache/Cache.hpp"
#include "domain/User.hpp"
#include "email/AccountEmails.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Audit.hpp"
#include "security/Auth.hpp"
#include "security/Password.hpp"
#include "security/RateLimit.hpp"
#include "security/SessionStore.hpp"
#include "security/Tokens.hpp"
#include "utils/Config.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Time.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AuthController : public HttpController<AuthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::registerUser, "/api/v1/auth/register", Post);
    ADD_METHOD_TO(AuthController::login, "/api/v1/auth/login", Post);
    ADD_METHOD_TO(AuthController::logout, "/api/v1/auth/logout", Post);
    ADD_METHOD_TO(AuthController::refresh, "/api/v1/auth/refresh", Post);
    ADD_METHOD_TO(AuthController::me, "/api/v1/auth/me", Get);
    METHOD_LIST_END

    // ---------------------------------------------------------------------
    // POST /api/auth/register
    //
    // Body: { email, password, first_name?, last_name? }
    // Behaviour: creates an unconfirmed user, generates a confirm-email
    // token, and (stage 2) emails it. NOT auto-login — flask-base parity:
    // user has to click the link, then log in.
    // ---------------------------------------------------------------------
    void registerUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "email");
        Validation::require(errs, body, "password");
        Validation::email(errs, body, "email");
        Validation::string_length(errs, body, "password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string email = body["email"].get<std::string>();
        const std::string password = body["password"].get<std::string>();
        const auto first_name = Validation::opt_string(body, "first_name");
        const auto last_name = Validation::opt_string(body, "last_name");

        Repositories::RoleRepository roles;
        auto default_role = roles.find_default();
        if (!default_role) {
            spdlog::error("No default role in DB — run migrations / setup-dev");
            callback(ErrorResponse::service_unavailable("misconfigured", "default role missing"));
            return;
        }

        // with_repo_errors centralizes the DuplicateEmail->409 / *->500 mapping
        // (was hand-rolled here, the exact drift the helper exists to prevent).
        with_repo_errors(callback, "register", [&] {
            const std::string hash = Security::Password::hash(password);
            Repositories::UserRepository users;
            auto created = users.create(email, hash, first_name, last_name, default_role->id, /*confirmed=*/false);

            // Attach the role we already loaded so to_json embeds it — no
            // need to re-query the row we just inserted.
            created.role = *default_role;
            // Fire the confirmation email. AccountEmails handles token
            // issuing + render + send; failures log but don't break
            // registration (the user still has an account, they can hit
            // /confirm-resend to retry).
            Email::AccountEmails::send_confirm(created);
            callback(Response::created({{"user", json(created)},
                                        {"message", "Account created. Check your email for the confirmation link."}}));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/auth/login
    //
    // Body: { email, password }
    // Returns: { user } + Set-Cookie access/refresh.
    // Generic 401 on either wrong email or wrong password (no user
    // enumeration). flask-base does the same thing with one flash message.
    // ---------------------------------------------------------------------
    void login(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "email");
        Validation::require(errs, body, "password");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string email = body["email"].get<std::string>();
        const std::string password = body["password"].get<std::string>();

        Repositories::UserRepository users;
        auto user = users.find_by_email(email);

        // Equalize timing across user-exists vs not. A missing user (or one with
        // no password hash) is verified against a fixed dummy hash so the ~90ms
        // argon2 cost is always paid — otherwise the short-circuit was a timing
        // oracle for user enumeration (argon2 is large enough to measure; DB
        // latency does not mask it). The dummy hash is computed once.
        static const std::string kDummyHash = Security::Password::hash("timing-equalizer-not-a-real-password");
        const std::string& hash_to_check = (user && user->password_hash) ? *user->password_hash : kDummyHash;
        const bool password_ok = Security::Password::verify(password, hash_to_check);

        if (!user || !user->password_hash || !password_ok) {
            // Audit the failed attempt so brute-force / credential-stuffing is
            // visible in the trail (it wasn't before — only successful admin
            // actions were recorded). No actor (unauthenticated); the attempted
            // email + source IP are the investigation handles. Use the shared
            // trusted-IP resolver (honors rate_limit.trust_proxy) — NOT a raw
            // X-Real-IP read, which is client-spoofable when not behind a proxy.
            const std::string ip = Security::RateLimit::client_ip(req);
            Security::Audit::record(
                /*actor_id=*/"", "auth.login_failed", "user", user ? user->id : "", {{"email", email}, {"ip", ip}});
            // Single message for missing-user + bad-password to defeat enumeration.
            callback(ErrorResponse::unauthorized("invalid_credentials", "Invalid email or password"));
            return;
        }

        // Issue access + refresh, write refresh JTI to Redis for revocation.
        auto session = mint_session(*user);
        if (!session) {
            callback(ErrorResponse::service_unavailable("session_unavailable", "Could not mint session"));
            return;
        }

        auto http = Response::ok({{"user", json(*user)}});
        Security::Auth::set_session_cookies(
            http, Security::Auth::get().config().cookies, session->access, session->refresh);
        callback(http);
    }

    // ---------------------------------------------------------------------
    // POST /api/auth/logout
    //
    // Reads refresh-token cookie, deletes its JTI from Redis (so further
    // /refresh calls fail), and zeroes both cookies.
    // ---------------------------------------------------------------------
    void logout(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto& cfg = Security::Auth::get().config();
        const std::string refresh = Security::Auth::extract_refresh_token(req, cfg.cookies);
        if (!refresh.empty())
            revoke_refresh(cfg, refresh);

        auto http = Response::ok({{"message", "logged out"}});
        Security::Auth::set_session_cookies(http, cfg.cookies, "", "");
        callback(http);
    }

    // ---------------------------------------------------------------------
    // POST /api/auth/refresh
    //
    // Reads refresh-token cookie, verifies it, checks the JTI is still
    // live in Redis, rotates: new access + new refresh (with new JTI),
    // deletes the old JTI. Returns the user payload.
    // ---------------------------------------------------------------------
    void refresh(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto& cfg = Security::Auth::get().config();
        const std::string refresh = Security::Auth::extract_refresh_token(req, cfg.cookies);
        if (refresh.empty()) {
            callback(ErrorResponse::unauthorized("missing_refresh"));
            return;
        }

        std::string err;
        auto claims_opt = Security::Auth::verify_hs256_jwt(refresh, cfg.jwt_secret, err);
        if (!claims_opt) {
            callback(ErrorResponse::unauthorized(err));
            return;
        }
        const auto& claims = *claims_opt;
        if (claims.value("typ", "") != "refresh") {
            callback(ErrorResponse::unauthorized("not_a_refresh"));
            return;
        }
        const std::string sub = claims.value("sub", "");
        const std::string jti = claims.value("jti", "");
        if (sub.empty() || jti.empty()) {
            callback(ErrorResponse::unauthorized("malformed_claims"));
            return;
        }

        // Revocation check.
        if (!is_refresh_live(cfg, jti)) {
            callback(ErrorResponse::unauthorized("revoked"));
            return;
        }

        Repositories::UserRepository users;
        auto user = users.find(sub);
        if (!user) {
            // User deleted while session was active.
            revoke_refresh(cfg, refresh);  // best effort
            callback(ErrorResponse::unauthorized("user_gone"));
            return;
        }

        // Rotate.
        revoke_jti(cfg, jti);
        auto session = mint_session(*user);
        if (!session) {
            callback(ErrorResponse::service_unavailable("session_unavailable"));
            return;
        }
        auto http = Response::ok({{"user", *user}});
        Security::Auth::set_session_cookies(http, cfg.cookies, session->access, session->refresh);
        callback(http);
    }

    // ---------------------------------------------------------------------
    // GET /api/auth/me
    //
    // Returns the authenticated user. Requires a valid access token (the
    // global auth middleware already gates the path). 401 if missing /
    // expired; 404 if the user row vanished mid-session.
    // ---------------------------------------------------------------------
    void me(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        auto principal = Security::Auth::principal_of(req);
        if (!principal) {
            callback(ErrorResponse::unauthorized("missing_principal"));
            return;
        }
        Repositories::UserRepository users;
        auto user = users.find(principal->subject);
        if (!user) {
            callback(ErrorResponse::not_found("user"));
            return;
        }
        callback(Response::ok({{"user", *user}}));
    }

private:
    struct Session {
        std::string access;
        std::string refresh;
    };

    static std::string make_jti() { return Utils::Crypto::random_hex(16); }

    /**
     * @brief Mint access + refresh JWTs, write refresh JTI to Redis.
     *        Returns nullopt if Redis write fails — refresh would be
     *        unverifiable, so we'd rather refuse the login than mint a
     *        permanently-invalid session.
     */
    std::optional<Session> mint_session(const Domain::User& user) {
        const auto& cfg = Security::Auth::get().config();
        if (cfg.jwt_secret.empty()) {
            spdlog::error("mint_session: JWT_SECRET unset");
            return std::nullopt;
        }
        const long now = Utils::Time::now_epoch_seconds();

        // Roles claim — string array, even for a single role, so the
        // existing AuthPrincipal extractor parses it consistently.
        json roles_array = json::array();
        if (user.role)
            roles_array.push_back(user.role->name);

        // Permissions bitmask in the JWT lets the request layer answer
        // require_permission(...) without re-loading the user from DB.
        // The bitmask matches Domain::Permission constants.
        const std::uint32_t perm_bits = user.role ? user.role->permissions : 0u;

        json access_claims = {
            {"sub", user.id},
            {"iat", now},
            {"exp", now + cfg.cookies.access_ttl_sec},
            {"typ", "access"},
            {"confirmed", user.confirmed},
            {"permissions", perm_bits},
            {cfg.jwt_roles_claim, roles_array},
        };
        if (!cfg.jwt_issuer.empty())
            access_claims["iss"] = cfg.jwt_issuer;
        if (!cfg.jwt_audience.empty())
            access_claims["aud"] = cfg.jwt_audience;

        const std::string jti = make_jti();
        json refresh_claims = {
            {"sub", user.id},
            {"iat", now},
            {"exp", now + cfg.cookies.refresh_ttl_sec},
            {"typ", "refresh"},
            {"jti", jti},
        };

        Session s;
        s.access = Security::Auth::issue_hs256_jwt(access_claims, cfg.jwt_secret);
        s.refresh = Security::Auth::issue_hs256_jwt(refresh_claims, cfg.jwt_secret);

        // Track the JTI (live-marker + per-user index for revoke-all). Redis
        // down → fail closed: a refresh we can't revoke is worse than a failed
        // login. record() returns false on the live-marker write failure.
        if (!Cache::is_initialized()) {
            spdlog::warn("Cache not initialized — refresh revocation will not work");
            return s;
        }
        if (!Security::Sessions::record(cfg.cookies, user.id, jti, cfg.cookies.refresh_ttl_sec)) {
            spdlog::error("mint_session: failed to record refresh JTI — refusing to mint session");
            return std::nullopt;
        }
        return s;
    }

    static bool is_refresh_live(const Security::Auth::AuthConfig& cfg, const std::string& jti) {
        return Security::Sessions::is_live(cfg.cookies, jti);
    }

    static void revoke_jti(const Security::Auth::AuthConfig& cfg, const std::string& jti) {
        Security::Sessions::revoke_jti(cfg.cookies, jti);
    }

    static void revoke_refresh(const Security::Auth::AuthConfig& cfg, const std::string& refresh_token) {
        std::string err;
        auto claims_opt = Security::Auth::verify_hs256_jwt(refresh_token, cfg.jwt_secret, err);
        if (!claims_opt)
            return;  // already invalid; nothing to revoke
        const std::string jti = claims_opt->value("jti", "");
        if (!jti.empty())
            revoke_jti(cfg, jti);
    }
};

}  // namespace Api
