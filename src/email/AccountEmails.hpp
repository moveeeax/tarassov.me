/**
 * @file AccountEmails.hpp
 * @brief Token-issuing email senders shared by Auth + Account + Admin
 *        controllers. The worker-side job handler lives in
 *        AccountEmailWorker.hpp.
 *
 * Pulled out of AccountController because AuthController::registerUser
 * needs send_confirm_email() too, and pulling AccountController into
 * Auth (or vice versa) would cycle the include graph.
 *
 * Delivery is routed, not inlined (flask-base parity: app/email.py
 * pushes onto Flask-RQ):
 *
 *   send_*()  ──Jobs enabled──▶ Jobs::submit("account_email", payload)
 *      │                              │
 *      │ Jobs off / submit failed     ▼  worker (AccountEmailWorker.hpp)
 *      └────────▶ deliver_now() ◀── process_job()
 *
 * The controller-facing send_*() helpers stay best-effort: enqueue (or
 * inline-send) failures are logged, the user is acked regardless, so
 * SMTP outages don't expose a timing oracle. On the worker, however,
 * deliver_now() failures THROW — Jobs::fail() then drives retries with
 * backoff and eventually the DLQ, giving at-least-once delivery.
 *
 * Tokens are issued in deliver_now(), i.e. at actual send time on the
 * worker, so TTLs aren't eaten by queue latency.
 */

#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "domain/User.hpp"
#include "email/Mailer.hpp"
#include "email/Templates.hpp"
#include "jobs/Jobs.hpp"
#include "security/Auth.hpp"
#include "security/Tokens.hpp"
#include "utils/Config.hpp"
#include "utils/Strings.hpp"

namespace Email::AccountEmails {

using json = nlohmann::json;

/// Job type the worker must subscribe to (WORKER_TYPES) to deliver mail.
inline constexpr const char* kJobType = "account_email";

namespace detail {

inline std::string master_secret() {
    if (!Security::Auth::is_initialized())
        return {};
    return Security::Auth::get().config().jwt_secret;
}

inline std::string base_url() {
    std::string base = "http://localhost:8080";
    if (Config::is_initialized())
        base = Config::get().get<std::string>("app.base_url", "APP_BASE_URL", base);
    if (!base.empty() && base.back() == '/')
        base.pop_back();
    return base;
}

inline json user_ctx(const Domain::User& u) {
    return json{
        {"email", u.email},
        {"full_name", u.full_name()},
        {"first_name", u.first_name.value_or("")},
        {"last_name", u.last_name.value_or("")},
    };
}

/**
 * @brief Render template pair, set From/To/Subject, ship via Mailer.
 *        THROWS on render failure or SMTP refusal — the worker relies
 *        on the exception to trigger retry/DLQ. Best-effort callers
 *        wrap this themselves.
 */
inline void render_and_send(const std::string& template_name,
                            const std::string& subject,
                            const std::string& to,
                            const json& ctx) {
    if (!Email::is_initialized())
        throw std::runtime_error("Mailer not initialized");
    auto rendered = Email::Templates::render_pair(template_name, ctx);
    Email::Message m;
    m.to = to;
    m.subject = subject;
    m.text_body = std::move(rendered.text);
    m.html_body = std::move(rendered.html);
    if (!Email::get().send(m))
        throw std::runtime_error("SMTP send failed for template " + template_name);
}

/**
 * @brief Issue the token, build the template context and send — NOW, in
 *        this process. Shared by the worker handler and the inline
 *        fallback. Throws on unknown kind, render or send failure.
 *
 * @param new_email Only meaningful for kind == "change_email": the
 *                  address being adopted (and the recipient).
 */
inline void deliver_now(const std::string& kind, const Domain::User& user, const std::string& new_email) {
    json ctx = Email::Templates::default_context();
    ctx["user"] = user_ctx(user);

    if (kind == "confirm") {
        const auto token = Security::Tokens::issue(
            master_secret(), user.id, Security::Tokens::Purpose::Confirm, std::chrono::hours(24 * 7));
        ctx["confirm_link"] = base_url() + "/account/confirm/" + token;
        render_and_send("confirm", "Confirm your account", user.email, ctx);
    } else if (kind == "reset_password") {
        const auto token = Security::Tokens::issue(
            master_secret(), user.id, Security::Tokens::Purpose::ResetPassword, std::chrono::hours(1));
        ctx["reset_link"] = base_url() + "/account/reset-password/" + token;
        render_and_send("reset_password", "Reset your password", user.email, ctx);
    } else if (kind == "change_email") {
        const auto token = Security::Tokens::issue(master_secret(),
                                                   user.id,
                                                   Security::Tokens::Purpose::ChangeEmail,
                                                   std::chrono::hours(1),
                                                   json{{"new_email", new_email}});
        ctx["new_email"] = new_email;
        ctx["change_email_link"] = base_url() + "/account/change-email/" + token;
        render_and_send("change_email", "Confirm your new email", new_email, ctx);
    } else if (kind == "invite") {
        const auto token = Security::Tokens::issue(
            master_secret(), user.id, Security::Tokens::Purpose::Invite, std::chrono::hours(24 * 7));
        ctx["invite_link"] = base_url() + "/account/join-from-invite/" + token;
        render_and_send("invite", "You're invited", user.email, ctx);
    } else {
        throw std::runtime_error("unknown account email kind: " + kind);
    }
}

inline bool via_jobs() {
    if (!Jobs::is_initialized())
        return false;
    if (Config::is_initialized())
        return Config::get().get<bool>("mail.via_jobs", "MAIL_VIA_JOBS", true);
    return true;
}

/**
 * @brief Controller-side entry: enqueue for the worker when Jobs is up
 *        (scales horizontally, survives SMTP hiccups via retry/DLQ);
 *        otherwise send inline. Both paths are best-effort from the
 *        caller's perspective — never throws.
 */
inline void dispatch(const std::string& kind, const Domain::User& user, const std::string& new_email = "") {
    if (via_jobs()) {
        try {
            json payload = {{"kind", kind}, {"user_id", user.id}};
            if (!new_email.empty())
                payload["new_email"] = new_email;
            auto job = Jobs::get().submit(kJobType, payload);
            spdlog::debug(
                "AccountEmails: {} for {} enqueued as job {}", kind, Utils::Strings::mask_email(user.email), job.id);
            return;
        } catch (const std::exception& e) {
            spdlog::warn("AccountEmails: enqueue {} for {} failed ({}); sending inline",
                         kind,
                         Utils::Strings::mask_email(user.email),
                         e.what());
        }
    }
    try {
        deliver_now(kind, user, new_email);
    } catch (const std::exception& e) {
        spdlog::warn(
            "AccountEmails: failed to send {} to {}: {}", kind, Utils::Strings::mask_email(user.email), e.what());
    }
}

}  // namespace detail

inline void send_confirm(const Domain::User& user) {
    detail::dispatch("confirm", user);
}

inline void send_reset(const Domain::User& user) {
    detail::dispatch("reset_password", user);
}

inline void send_change_email(const Domain::User& user, const std::string& new_email) {
    detail::dispatch("change_email", user, new_email);
}

inline void send_invite(const Domain::User& user) {
    detail::dispatch("invite", user);
}

}  // namespace Email::AccountEmails
