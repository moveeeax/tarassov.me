/**
 * @file AccountController.hpp
 * @brief Account self-service: confirm-email, reset-password, change-email,
 *        change-password, resend-confirm.
 *
 * flask-base parity: app/account/views.py — same flows, JSON endpoints
 * instead of HTML+flash. Token routes accept the token as a path
 * segment so links in emails Just Work without query-string mangling.
 *
 * All these handlers are intentionally minimal — render template,
 * issue Tokens, persist via UserRepository. No business logic beyond
 * what flask-base already specified.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "domain/User.hpp"
#include "email/AccountEmails.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "security/Password.hpp"
#include "security/SessionStore.hpp"
#include "security/Tokens.hpp"
#include "utils/Config.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AccountController : public HttpController<AccountController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AccountController::resendConfirm, "/api/v1/account/confirm-resend", Post);
    ADD_METHOD_TO(AccountController::confirm, "/api/v1/account/confirm/{1}", Post);
    ADD_METHOD_TO(AccountController::requestReset, "/api/v1/account/reset-password-request", Post);
    ADD_METHOD_TO(AccountController::applyReset, "/api/v1/account/reset-password/{1}", Post);
    ADD_METHOD_TO(AccountController::requestChangeEmail, "/api/v1/account/change-email-request", Post);
    ADD_METHOD_TO(AccountController::applyChangeEmail, "/api/v1/account/change-email/{1}", Post);
    ADD_METHOD_TO(AccountController::joinFromInvite, "/api/v1/account/join-from-invite/{1}", Post);
    ADD_METHOD_TO(AccountController::changePassword, "/api/v1/account/change-password", Post);
    METHOD_LIST_END

    // ---------------------------------------------------------------------
    // POST /api/account/confirm-resend  (auth required)
    //
    // Re-send the confirmation email for the *current* user. Idempotent:
    // even an already-confirmed user can request, but we return 200 with
    // a no-op message rather than firing a redundant token.
    // ---------------------------------------------------------------------
    void resendConfirm(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        try {
            Repositories::UserRepository repo;
            auto user = repo.find(principal->subject);
            if (!user) {
                callback(ErrorResponse::not_found("user"));
                return;
            }
            if (user->confirmed) {
                callback(Response::ok({{"message", "already confirmed"}}));
                return;
            }
            Email::AccountEmails::send_confirm(*user);
            callback(Response::ok({{"message", "confirmation email sent"}}));
        } catch (const std::exception& e) {
            spdlog::error("resendConfirm failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    // ---------------------------------------------------------------------
    // POST /api/account/confirm/{token}
    //
    // Public — the link in the email points here. Verifies token against
    // Confirm purpose, marks user confirmed.
    // ---------------------------------------------------------------------
    void confirm(const HttpRequestPtr& /*req*/,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& token) {
        auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::Confirm);
        if (!vr.ok) {
            callback(ErrorResponse::bad_request("invalid_token", "Confirmation link is invalid or has expired"));
            return;
        }
        // One-shot, consistent with applyReset/applyChangeEmail: a captured
        // confirm link shouldn't stay replayable for the token's full (7-day)
        // lifetime. TTL matches the confirm token lifetime so the replay guard
        // outlives the token it protects.
        if (!consume_once(token, /*ttl_sec=*/7 * 24 * 3600)) {
            callback(ErrorResponse::bad_request("invalid_token", "Confirmation link has already been used"));
            return;
        }
        try {
            Repositories::UserRepository repo;
            if (!repo.mark_confirmed(vr.sub)) {
                callback(ErrorResponse::not_found("user"));
                return;
            }
            callback(Response::ok({{"message", "Account confirmed"}}));
        } catch (const std::exception& e) {
            spdlog::error("confirm failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    // ---------------------------------------------------------------------
    // POST /api/account/reset-password-request
    //
    // Public. Body: { email }. Always returns 200 — we never reveal
    // whether the email is registered (flask-base does the same with
    // its flash message wording).
    // ---------------------------------------------------------------------
    void requestReset(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "email");
        Validation::email(errs, body, "email");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        const std::string email = body["email"].get<std::string>();
        try {
            Repositories::UserRepository repo;
            auto user = repo.find_by_email(email);
            if (user) {
                Email::AccountEmails::send_reset(*user);
            } else {
                // Debug level, no address: the probed email is PII and an
                // info-level log would be a log-side enumeration channel
                // undercutting the deliberate generic 200 below.
                spdlog::debug("[reset-request] no matching user — silent ack");
            }
        } catch (const std::exception& e) {
            // Same generic 200 on backend trouble — a 500 here would leak
            // that the lookup ran (and retrying costs the user nothing).
            spdlog::error("requestReset failed: {}", e.what());
        }
        // Generic 200 either way — no enumeration.
        callback(Response::ok({{"message", "If that email is registered, a reset link is on its way."}}));
    }

    // ---------------------------------------------------------------------
    // POST /api/account/reset-password/{token}
    //
    // Body: { new_password }. Verifies token, sets a fresh password hash.
    // ---------------------------------------------------------------------
    void applyReset(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& token) {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "new_password");
        Validation::string_length(errs, body, "new_password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::ResetPassword);
        if (!vr.ok) {
            callback(ErrorResponse::bad_request("invalid_token", "Reset link is invalid or has expired"));
            return;
        }
        // One-shot: a captured reset link must not be replayable to set the
        // password a second time after the legitimate user used it.
        if (!consume_once(token, /*ttl_sec=*/3600)) {
            callback(ErrorResponse::bad_request("invalid_token", "Reset link has already been used"));
            return;
        }
        with_repo_errors(callback, "applyReset", [&] {
            Repositories::UserRepository repo;
            const std::string new_hash = Security::Password::hash(body["new_password"].get<std::string>());
            repo.update_password_hash(vr.sub, new_hash);
            // Evict every existing session — a reset must lock out anyone
            // holding an old refresh token (incl. an attacker who triggered
            // the reset path). Best-effort; the new login mints a fresh one.
            Security::Sessions::revoke_all(Security::Auth::get().config().cookies, vr.sub);
            callback(Response::ok({{"message", "Password updated"}}));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/account/change-email-request   (auth required)
    //
    // Body: { new_email, password }. Verifies current password, mints
    // a token bearing the new_email, sends confirmation email to the
    // *new* address (not the old one — flask-base behaviour).
    // ---------------------------------------------------------------------
    void requestChangeEmail(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "new_email");
        Validation::email(errs, body, "new_email");
        Validation::require(errs, body, "password");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        try {
            Repositories::UserRepository repo;
            auto user = repo.find(principal->subject);
            if (!user || !user->password_hash) {
                callback(ErrorResponse::unauthorized("invalid_credentials"));
                return;
            }
            if (!Security::Password::verify(body["password"].get<std::string>(), *user->password_hash)) {
                callback(ErrorResponse::unauthorized("invalid_credentials", "Wrong password"));
                return;
            }
            const std::string new_email = body["new_email"].get<std::string>();
            Email::AccountEmails::send_change_email(*user, new_email);
            callback(Response::ok({{"message", "Confirmation email sent to the new address."}}));
        } catch (const std::exception& e) {
            spdlog::error("requestChangeEmail failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    // ---------------------------------------------------------------------
    // POST /api/account/change-email/{token}
    //
    // Verifies token, changes email atomically. Fails 409 if the new
    // address has been registered by someone else in the meantime.
    // ---------------------------------------------------------------------
    void applyChangeEmail(const HttpRequestPtr& /*req*/,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& token) {
        auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::ChangeEmail);
        if (!vr.ok) {
            callback(ErrorResponse::bad_request("invalid_token", "Change-email link is invalid or has expired"));
            return;
        }
        const auto new_email_it = vr.extra.find("new_email");
        if (new_email_it == vr.extra.end() || !new_email_it->is_string()) {
            callback(ErrorResponse::bad_request("invalid_token", "Token is missing the new email"));
            return;
        }
        const std::string new_email = new_email_it->get<std::string>();
        // Check availability BEFORE consuming the one-shot token, so a
        // duplicate-email 409 doesn't permanently burn an otherwise-valid link.
        // change_email's UNIQUE constraint still guards the check→write race.
        {
            Repositories::UserRepository repo;
            auto taken = repo.find_by_email(new_email);
            if (taken && taken->id != vr.sub) {
                callback(ErrorResponse::make(
                    {drogon::k409Conflict, "email_taken", "That email address is already in use", nlohmann::json{}}));
                return;
            }
        }
        if (!consume_once(token, /*ttl_sec=*/3600)) {
            callback(ErrorResponse::bad_request("invalid_token", "Change-email link has already been used"));
            return;
        }
        with_repo_errors(callback, "applyChangeEmail", [&] {
            Repositories::UserRepository repo;
            repo.change_email(vr.sub, new_email);
            callback(Response::ok({{"message", "Email updated"}}));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/account/join-from-invite/{token}
    //
    // Public — the link in the admin invitation email points here. Verifies
    // the Invite token, sets the invitee's first password and confirms the
    // account in a single write. flask-base parity: /join-from-invite/<token>.
    // ---------------------------------------------------------------------
    void joinFromInvite(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& token) {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "new_password");
        Validation::string_length(errs, body, "new_password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::Invite);
        if (!vr.ok) {
            callback(ErrorResponse::bad_request("invalid_token", "Invitation link is invalid or has expired"));
            return;
        }
        with_repo_errors(callback, "joinFromInvite", [&] {
            Repositories::UserRepository repo;
            const std::string new_hash = Security::Password::hash(body["new_password"].get<std::string>());
            // redeem_invite is a DB-level one-shot (only matches a still-pending
            // invite), so a replayed link can't reset an active account — no
            // need for the fail-open Redis guard here. Doing the write first
            // also means a transient failure doesn't burn the 7-day token.
            if (!repo.redeem_invite(vr.sub, new_hash)) {
                callback(ErrorResponse::bad_request("invalid_token",
                                                    "This invitation has already been used or is no longer valid"));
                return;
            }
            // Parity with applyReset: drop any sessions for this subject.
            Security::Sessions::revoke_all(Security::Auth::get().config().cookies, vr.sub);
            callback(Response::ok({{"message", "Account ready — you can now sign in."}}));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/account/change-password   (auth required)
    //
    // Body: { old_password, new_password }. Verifies the old password
    // against the stored hash; updates the hash. Doesn't invalidate
    // existing sessions — that's a deliberate trade-off matching
    // flask-base. If you need session-rotation, mint a fresh refresh
    // pair after the change.
    // ---------------------------------------------------------------------
    void changePassword(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "old_password");
        Validation::require(errs, body, "new_password");
        Validation::string_length(errs, body, "new_password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        try {
            Repositories::UserRepository repo;
            auto user = repo.find(principal->subject);
            if (!user || !user->password_hash) {
                callback(ErrorResponse::unauthorized("invalid_credentials"));
                return;
            }
            if (!Security::Password::verify(body["old_password"].get<std::string>(), *user->password_hash)) {
                callback(ErrorResponse::unauthorized("invalid_credentials", "Original password is incorrect"));
                return;
            }
            const std::string new_hash = Security::Password::hash(body["new_password"].get<std::string>());
            repo.update_password_hash(user->id, new_hash);
            // Revoke other sessions on password change (the current client
            // will re-auth on its next refresh). Closes the "changed my
            // password but the thief stays logged in" gap.
            Security::Sessions::revoke_all(Security::Auth::get().config().cookies, user->id);
            callback(Response::ok({{"message", "Password updated"}}));
        } catch (const std::exception& e) {
            spdlog::error("changePassword failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

private:
    static std::string secret() { return Security::Auth::get().config().jwt_secret; }

    /**
     * @brief Atomically consume a one-shot token: returns true the FIRST time
     *        this token is seen, false on every replay. DB-authoritative via the
     *        used_tokens table (migration 002) + INSERT ... ON CONFLICT DO
     *        NOTHING — so a captured token CANNOT be replayed during a Redis
     *        outage the way the old cache-only nonce (fail-open) allowed.
     *        Fail-CLOSED on a DB error (returns false): the guarded action needs
     *        the DB anyway, so refusing the token is the safe choice. Never
     *        throws — callers consume it outside with_repo_errors.
     */
    static bool consume_once(const std::string& token, int ttl_sec) {
        try {
            const std::string hash = Utils::Crypto::sha256_hex(token);
            return Database::get().execute_write([&](auto& txn) {
                auto r = txn.exec_params(
                    "INSERT INTO used_tokens (token_hash, expires_at) "
                    "VALUES ($1, now() + make_interval(secs => $2)) "
                    "ON CONFLICT (token_hash) DO NOTHING RETURNING token_hash",
                    hash,
                    ttl_sec);
                return !r.empty();  // a row came back ⇒ this is the first use
            });
        } catch (const std::exception& e) {
            spdlog::warn("consume_once: nonce write failed ({}) — refusing token (fail-closed)", e.what());
            return false;
        }
    }
};

}  // namespace Api
