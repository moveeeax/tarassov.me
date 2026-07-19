/**
 * @file ContactController.hpp
 * @brief Public contact form: POST /api/v1/public/contact emails the site owner
 *        via the existing Mailer (Email::SendEmail — enqueued through Jobs when
 *        available, else inline). Unauthenticated; throttled by the general rate
 *        limiter. The recipient comes from config mail.contact_to / CONTACT_EMAIL;
 *        when unset the endpoint reports the form is not configured.
 */

#pragma once

#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json.hpp>

#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "email/GenericEmail.hpp"
#include "utils/Config.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class ContactController : public HttpController<ContactController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ContactController::submit, "/api/v1/public/contact", Post);
    METHOD_LIST_END

    void submit(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "name");
        Validation::require(errs, body, "email");
        Validation::require(errs, body, "message");
        Validation::email(errs, body, "email");
        Validation::string_length(errs, body, "name", 1, 120);
        Validation::string_length(errs, body, "subject", 0, 200);
        Validation::string_length(errs, body, "message", 1, 5000);
        // name/subject can reach an email header → reject CR/LF (header injection).
        Validation::no_crlf(errs, body, "name");
        Validation::no_crlf(errs, body, "subject");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string to =
            Config::is_initialized() ? Config::get().get<std::string>("mail.contact_to", "CONTACT_EMAIL", "") : "";
        if (to.empty()) {
            callback(ErrorResponse::service_unavailable("contact_disabled", "Contact form is not configured"));
            return;
        }

        const auto name = body["name"].get<std::string>();
        const auto email = body["email"].get<std::string>();
        const auto subject = body.value("subject", std::string{"(no subject)"});
        const auto message = body["message"].get<std::string>();

        // The submitter goes into the body AND into Reply-To, so the owner can
        // just hit "Reply" (the envelope From must stay the app's MAIL_FROM —
        // SPF/DKIM are aligned to our domain, not the submitter's).
        const std::string text = "From: " + name + " <" + email + ">\n\n" + message;
        Email::SendEmail::send(to, "[Contact] " + subject, text, /*html=*/"", /*reply_to=*/email);

        callback(Response::ok({{"message", "Thanks — your message has been sent."}}));
    }
};

}  // namespace Api
