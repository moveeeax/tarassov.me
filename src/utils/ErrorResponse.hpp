/**
 * @file ErrorResponse.hpp
 * @brief Unified JSON error body used by every controller and middleware.
 * @details One stable shape everywhere:
 * @code
 *   {
 *     "error":   "<machine-readable code>",
 *     "status":  <http status int>,
 *     "message": "<optional human message>",
 *     // extra fields merged in at top level, e.g.:
 *     "errors":          [...],    // validation
 *     "retry_after_sec": N,        // rate limit
 *     "code":            "..."     // auth
 *   }
 * @endcode
 *
 * Avoid building HttpResponse + JSON body ad-hoc in controllers — use the
 * helpers below so the frontend always parses the same shape.
 */

#pragma once

#include <string>
#include <utility>

#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

namespace ErrorResponse {

using json = nlohmann::json;

struct Error {
    drogon::HttpStatusCode status;
    std::string code;             // stable machine code: "not_found", "validation_failed", ...
    std::string message;          // optional human-readable detail
    json extra = json::object();  // optional extra keys merged at top level
};

inline drogon::HttpResponsePtr make(Error e) {
    json body = {{"error", e.code}, {"status", static_cast<int>(e.status)}};
    if (!e.message.empty())
        body["message"] = std::move(e.message);
    if (e.extra.is_object()) {
        for (auto it = e.extra.begin(); it != e.extra.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(e.status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

// ---- shorthand builders ---------------------------------------------------

inline drogon::HttpResponsePtr bad_request(std::string code, std::string message = "", json extra = json::object()) {
    return make({drogon::k400BadRequest, std::move(code), std::move(message), std::move(extra)});
}

inline drogon::HttpResponsePtr unauthorized(std::string code = "unauthorized", std::string message = "") {
    // The WWW-Authenticate challenge header is added by the auth middleware
    // (Api.hpp::register_auth), which has the scheme/error context — keep the
    // body builder pure here.
    return make({drogon::k401Unauthorized, std::move(code), std::move(message), json::object()});
}

inline drogon::HttpResponsePtr forbidden(std::string code = "forbidden", std::string message = "") {
    return make({drogon::k403Forbidden, std::move(code), std::move(message), json::object()});
}

inline drogon::HttpResponsePtr not_found(std::string what = "resource") {
    return make({drogon::k404NotFound, "not_found", what + " not found", json::object()});
}

inline drogon::HttpResponsePtr conflict(std::string code, std::string message = "") {
    return make({drogon::k409Conflict, std::move(code), std::move(message), json::object()});
}

inline drogon::HttpResponsePtr payload_too_large(std::string code = "payload_too_large") {
    return make({drogon::k413RequestEntityTooLarge, std::move(code), "", json::object()});
}

inline drogon::HttpResponsePtr unsupported_media_type(std::string code = "unsupported_media_type",
                                                      std::string message = "") {
    return make({drogon::k415UnsupportedMediaType, std::move(code), std::move(message), json::object()});
}

inline drogon::HttpResponsePtr unprocessable(std::string code, std::string message = "") {
    return make({drogon::k422UnprocessableEntity, std::move(code), std::move(message), json::object()});
}

inline drogon::HttpResponsePtr too_many_requests(int retry_after_sec) {
    return make({drogon::k429TooManyRequests, "rate_limited", "", json{{"retry_after_sec", retry_after_sec}}});
}

inline drogon::HttpResponsePtr internal_error(std::string code = "internal_error", std::string message = "") {
    return make({drogon::k500InternalServerError, std::move(code), std::move(message), json::object()});
}

inline drogon::HttpResponsePtr service_unavailable(std::string code = "service_unavailable", std::string message = "") {
    return make({drogon::k503ServiceUnavailable, std::move(code), std::move(message), json::object()});
}

}  // namespace ErrorResponse

namespace Response {

using json = nlohmann::json;

/**
 * @brief Build a 200 application/json response from a JSON body.
 *        Centralized so controllers don't repeat newHttpResponse + setBody +
 *        setContentTypeCode + setStatusCode in every handler.
 */
inline drogon::HttpResponsePtr ok(const json& body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(body.dump());
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setStatusCode(drogon::k200OK);
    return resp;
}

/// 201 Created variant.
inline drogon::HttpResponsePtr created(const json& body) {
    auto resp = ok(body);
    resp->setStatusCode(drogon::k201Created);
    return resp;
}

/**
 * @brief 200 with the standard paginated list envelope: one shape every list
 *        endpoint should emit so clients can rely on it. @p data must be a JSON
 *        array. Pairs with RequestUtils::parse_page_params.
 */
inline drogon::HttpResponsePtr paginated(const json& data, long total, int limit, int offset) {
    return ok({{"data", data}, {"total", total}, {"limit", limit}, {"offset", offset}});
}

/// 200 with a bare collection envelope: {data:[...]} (unpaginated lists).
inline drogon::HttpResponsePtr list(const json& data) {
    return ok({{"data", data}});
}

}  // namespace Response
