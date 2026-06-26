/**
 * @file Validation.hpp
 * @brief Composable request-body validators.
 * @details Controllers accumulate field errors into a @c Validation::Errors
 *          collector and emit a single 400 response with a structured error
 *          list. Keeps validation logic out of each controller's imperative
 *          tree of `if (…) return Response::ok(err, 400);`.
 *
 *          Response shape:
 *          @code
 *          {
 *            "error": "validation_failed",
 *            "errors": [
 *              {"field": "email", "code": "missing", "message": "required"},
 *              {"field": "username", "code": "too_short", "message": "min 1"}
 *            ]
 *          }
 *          @endcode
 */

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

#include "utils/ErrorResponse.hpp"

namespace Api::Validation {

using json = nlohmann::json;

// Password length bounds — single source for register / reset-password /
// change-password / admin-create. Mirrored by the zod schema on the frontend
// (frontend/src/lib/schemas/auth.ts).
inline constexpr std::size_t kPasswordMinLen = 8;
inline constexpr std::size_t kPasswordMaxLen = 128;

struct Error {
    std::string field;
    std::string code;
    std::string message;
};

/**
 * @brief Accumulator that each validator appends to.
 */
class Errors {
public:
    void add(std::string field, std::string code, std::string message) {
        items_.push_back({std::move(field), std::move(code), std::move(message)});
    }
    bool any() const { return !items_.empty(); }
    const std::vector<Error>& items() const { return items_; }

    // Build the `errors` array only — the full response body is assembled by
    // response_400() using the shared ErrorResponse shape.
    json errors_json() const {
        json arr = json::array();
        for (const auto& e : items_) {
            arr.push_back({{"field", e.field}, {"code", e.code}, {"message", e.message}});
        }
        return arr;
    }

private:
    std::vector<Error> items_;
};

// ---------------------------------------------------------------------------
// Field validators — each inspects body[field] and appends on failure.
// Every validator is a no-op on a field that doesn't exist (use require()
// first to enforce presence).
// ---------------------------------------------------------------------------

/**
 * @brief Require a field to be present AND a non-null value.
 */
inline bool require(Errors& errs, const json& body, const std::string& field) {
    if (!body.contains(field) || body[field].is_null()) {
        errs.add(field, "missing", "required");
        return false;
    }
    return true;
}

/**
 * @brief Require a string field within [min, max] length. Non-strings fail
 *        with code "not_string". Missing fields are no-op (pair with require).
 */
inline void string_length(Errors& errs, const json& body, const std::string& field, size_t min_len, size_t max_len) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return;
    }
    const auto& s = body[field].get_ref<const std::string&>();
    if (s.size() < min_len) {
        errs.add(field, "too_short", "min length " + std::to_string(min_len));
    } else if (s.size() > max_len) {
        errs.add(field, "too_long", "max length " + std::to_string(max_len));
    }
}

/**
 * @brief Require a string field to match the given regex.
 */
inline void regex_match(
    Errors& errs, const json& body, const std::string& field, const std::regex& re, const std::string& format_hint) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return;
    }
    const auto& s = body[field].get_ref<const std::string&>();
    if (!std::regex_match(s, re)) {
        errs.add(field, "bad_format", "expected format: " + format_hint);
    }
}

/**
 * @brief Require an integer field within [min, max].
 */
inline void int_range(Errors& errs, const json& body, const std::string& field, long long min_val, long long max_val) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_number_integer()) {
        errs.add(field, "not_integer", "must be an integer");
        return;
    }
    auto v = body[field].get<long long>();
    if (v < min_val) {
        errs.add(field, "below_min", "min " + std::to_string(min_val));
    } else if (v > max_val) {
        errs.add(field, "above_max", "max " + std::to_string(max_val));
    }
}

/**
 * @brief Require a string field to be one of a fixed set of values.
 */
inline void one_of(Errors& errs, const json& body, const std::string& field, const std::vector<std::string>& allowed) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return;
    }
    const auto& s = body[field].get_ref<const std::string&>();
    for (const auto& a : allowed)
        if (s == a)
            return;
    std::string msg = "must be one of:";
    for (const auto& a : allowed) {
        msg += " '" + a + "'";
    }
    errs.add(field, "not_allowed", std::move(msg));
}

/**
 * @brief Pull an optional string field. Returns nullopt when the field is
 *        absent or not a string — collapses the
 *        `if (body.contains(f) && body[f].is_string()) x = body[f]...`
 *        boilerplate that recurs across the auth/admin controllers.
 */
inline std::optional<std::string> opt_string(const json& body, const std::string& field) {
    if (body.contains(field) && body[field].is_string())
        return body[field].get<std::string>();
    return std::nullopt;
}

inline void email(Errors& errs, const json& body, const std::string& field) {
    // Pragmatic email regex — not a spec-compliant RFC 5322 parser, but
    // rejects the obvious bad inputs without blocking valid ones.
    static const std::regex re(R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
    regex_match(errs, body, field, re, "email");
}

inline void uuid(Errors& errs, const json& body, const std::string& field) {
    static const std::regex re("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    regex_match(errs, body, field, re, "uuid");
}

// ---------------------------------------------------------------------------
// Response helper
// ---------------------------------------------------------------------------

inline drogon::HttpResponsePtr response_400(const Errors& errs) {
    return ErrorResponse::bad_request("validation_failed", "", json{{"errors", errs.errors_json()}});
}

/**
 * @brief Parse the request body as JSON. Returns true on success and fills
 *        @p out; on failure invokes @p cb with a 400 response and returns false.
 *        Eliminates the 5-line try/catch boilerplate in every mutating handler.
 *
 * Usage:
 * @code
 *   json body;
 *   if (!Validation::parse_body(req, body, callback)) return;
 * @endcode
 */
inline bool parse_body(const drogon::HttpRequestPtr& req,
                       json& out,
                       std::function<void(const drogon::HttpResponsePtr&)>& cb) {
    try {
        out = json::parse(std::string(req->body()));
        return true;
    } catch (...) {
        cb(ErrorResponse::bad_request("invalid_json", "Invalid JSON body"));
        return false;
    }
}

}  // namespace Api::Validation
