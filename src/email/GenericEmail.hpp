/**
 * @file GenericEmail.hpp
 * @brief Fire-and-forget transactional email for ANY app code — not tied to the
 *        account flows. `Email::SendEmail::send(to, subject, text, html)`
 *        enqueues an "email.send" job (retry/backoff/DLQ via Jobs, horizontal
 *        scale on the worker) when Jobs is up, else sends inline. Best-effort
 *        from the caller's side; the worker-side handler THROWS on SMTP refusal
 *        so Jobs drives at-least-once delivery.
 *
 * Mirrors the AccountEmails enqueue/deliver split — see AccountEmails.hpp.
 */

#pragma once

#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "email/Mailer.hpp"
#include "jobs/Jobs.hpp"
#include "utils/Config.hpp"
#include "utils/Strings.hpp"

namespace Email::SendEmail {

using json = nlohmann::json;

/// Job type the worker subscribes to (WORKER_TYPES) to deliver ad-hoc mail.
inline constexpr const char* kJobType = "email.send";

/// Build a Message from the job payload. Validates required fields up front.
inline Email::Message message_from_payload(const json& payload) {
    Email::Message m;
    m.to = payload.value("to", "");
    m.subject = payload.value("subject", "");
    m.text_body = payload.value("text", "");
    m.html_body = payload.value("html", "");
    m.reply_to = payload.value("reply_to", "");
    if (m.to.empty())
        throw std::runtime_error("email.send: 'to' is required");
    if (m.text_body.empty() && m.html_body.empty())
        throw std::runtime_error("email.send: needs a 'text' or 'html' body");
    return m;
}

/**
 * @brief Worker-side: deliver one ad-hoc email. THROWS on render/SMTP refusal so
 *        Jobs::fail() drives retry/backoff and eventually the DLQ.
 */
inline json process_job(const json& payload) {
    Email::Message m = message_from_payload(payload);
    if (!Email::is_initialized())
        throw std::runtime_error("email.send: Mailer not initialized");
    if (!Email::get().send(m))
        throw std::runtime_error("email.send: SMTP refused mail to " + m.to);
    return json{{"sent", true}, {"to", m.to}};
}

inline bool via_jobs() {
    if (!Jobs::is_initialized())
        return false;
    if (Config::is_initialized())
        return Config::get().get<bool>("mail.via_jobs", "MAIL_VIA_JOBS", true);
    return true;
}

/**
 * @brief App-facing entry: send an arbitrary email. Enqueue for the worker when
 *        Jobs is up (retry/DLQ, scales out), else send inline. Best-effort —
 *        NEVER throws, so a transactional path can fire it without a try/catch.
 */
inline void send(const std::string& to,
                 const std::string& subject,
                 const std::string& text,
                 const std::string& html = "",
                 const std::string& reply_to = "") {
    if (via_jobs()) {
        try {
            json payload = {{"to", to}, {"subject", subject}, {"text", text}};
            if (!html.empty())
                payload["html"] = html;
            if (!reply_to.empty())
                payload["reply_to"] = reply_to;
            auto job = Jobs::get().submit(kJobType, payload);
            spdlog::debug("SendEmail: to {} enqueued as job {}", Utils::Strings::mask_email(to), job.id);
            return;
        } catch (const std::exception& e) {
            spdlog::warn(
                "SendEmail: enqueue to {} failed ({}); sending inline", Utils::Strings::mask_email(to), e.what());
        }
    }
    try {
        if (!Email::is_initialized()) {
            spdlog::warn("SendEmail: mailer not initialized; dropping mail to {}", Utils::Strings::mask_email(to));
            return;
        }
        Email::Message m;
        m.to = to;
        m.subject = subject;
        m.text_body = text;
        m.html_body = html;
        m.reply_to = reply_to;
        if (!Email::get().send(m))
            spdlog::warn("SendEmail: SMTP refused mail to {}", Utils::Strings::mask_email(to));
    } catch (const std::exception& e) {
        spdlog::warn("SendEmail: failed to send to {}: {}", Utils::Strings::mask_email(to), e.what());
    }
}

}  // namespace Email::SendEmail
