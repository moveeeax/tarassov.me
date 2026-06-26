/**
 * @file Middleware.hpp
 * @brief Drogon advice chain: content-type check, auth, rate limit,
 *        idempotency, CORS, tracing, access log + HTTP metrics, and the
 *        optional Swagger UI endpoints.
 * @details Registration order matters and is owned by
 *          Api::register_controllers() in Api.hpp. This header is the only
 *          API-layer file that pulls the OTel SDK — controllers stay light.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span_context.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/RequestUtils.hpp"
#include "observability/Observability.hpp"
#include "observability/Trace.hpp"
#include "security/ApiKeys.hpp"
#include "security/Auth.hpp"
#include "security/Csrf.hpp"
#include "security/Idempotency.hpp"
#include "security/RateLimit.hpp"
#include "utils/Config.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Strings.hpp"

namespace Api {

/**
 * @brief HTTP metric families. Lazily created by ensure_http_metric_families().
 */
inline prometheus::Family<prometheus::Counter>* http_requests_family = nullptr;
inline prometheus::Family<prometheus::Histogram>* http_duration_family = nullptr;
inline const std::vector<double> HTTP_DURATION_BUCKETS = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0};

namespace middleware {

namespace detail {

using TraceSpan = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;

/**
 * @brief Hex string → fixed-size byte buffer (for TraceId/SpanId).
 * @return false if the input length doesn't match or has a non-hex char.
 */
inline bool hex_to_bytes(std::string_view hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2)
        return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

/**
 * @brief Build a remote OTel SpanContext from a parsed W3C traceparent, so
 *        our server span JOINS the caller's distributed trace instead of
 *        starting an unrelated root.
 */
inline std::optional<opentelemetry::trace::SpanContext> to_remote_span_context(
    const Observability::Trace::TraceContext& t) {
    uint8_t tid[16], sid[8], flags[1];
    if (!hex_to_bytes(t.trace_id, tid, 16) || !hex_to_bytes(t.parent_id, sid, 8) || !hex_to_bytes(t.flags, flags, 1)) {
        return std::nullopt;
    }
    return opentelemetry::trace::SpanContext(
        opentelemetry::trace::TraceId(opentelemetry::nostd::span<const uint8_t, 16>(tid)),
        opentelemetry::trace::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(sid)),
        opentelemetry::trace::TraceFlags(flags[0]),
        /*is_remote=*/true);
}

}  // namespace detail

inline void ensure_http_metric_families() {
    if (!Observability::is_initialized())
        return;
    auto& metrics = Observability::get().metrics();
    http_requests_family =
        &metrics.create_counter("http_requests_total", "Total HTTP requests by method, path, and status code");
    http_duration_family = &metrics.create_histogram("http_request_duration_seconds",
                                                     "HTTP request duration in seconds by method and path");
}

inline void register_auth() {
    if (!Security::Auth::is_initialized())
        return;
    if (Security::Auth::get().config().mode == Security::Auth::AuthMode::None)
        return;

    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        // CORS preflight (OPTIONS) carries no credentials and is answered by the
        // CORS advice — never gate it behind auth, or the browser preflight gets
        // a 401 and the actual request is never sent.
        if (req->method() == drogon::Options)
            return {};
        auto& auth = Security::Auth::get();
        const auto& cfg = auth.config();
        if (auth.path_is_public(req->path()))
            return {};

        auto unauthorized = [&](const std::string& code) {
            auto resp = ErrorResponse::unauthorized(code);
            resp->addHeader("WWW-Authenticate", "Bearer error=\"" + code + "\"");
            return resp;
        };

        if (cfg.mode == Security::Auth::AuthMode::Bearer) {
            // Bearer mode is the legacy header-only path — kept verbatim.
            const auto& header = req->getHeader("Authorization");
            const std::string expected = "Bearer " + cfg.bearer_token;
            // Constant-time compare: a plain `==` returns on the first differing
            // byte, leaking the static token through response timing.
            return Utils::Crypto::constant_time_equals(header, expected) ? drogon::HttpResponsePtr{}
                                                                         : unauthorized("invalid_token");
        }
        // Machine clients: a presented API key (X-API-Key, or an Authorization
        // token with the cpk_ prefix) fully decides the request. Absent → fall
        // through to the JWT/cookie path below. Honored in JWT mode only.
        if (Security::ApiKeys::request_has_key(req)) {
            auto key_principal = Security::ApiKeys::authenticate(req);
            if (!key_principal)
                return unauthorized("invalid_api_key");
            req->attributes()->insert(Security::Auth::kPrincipalAttr, *key_principal);
            return {};
        }
        // JWT — accept either the Authorization header or the configured
        // access cookie (cookie wins; SPAs never send the header). The
        // helper handles the Bearer prefix internally.
        std::string token = Security::Auth::extract_access_token(req, cfg.cookies);
        if (token.empty())
            return unauthorized("missing_token");

        std::string err;
        auto principal = auth.verify_jwt(token, err);
        if (!principal)
            return unauthorized(err);
        req->attributes()->insert(Security::Auth::kPrincipalAttr, *principal);
        return {};
    });
}

inline void register_rate_limit() {
    if (!Security::RateLimit::is_initialized())
        return;
    if (!Security::RateLimit::get().config().enabled)
        return;

    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        // Don't rate-limit CORS preflight (OPTIONS) — a throttled IP would get
        // 429 on the preflight and the browser would block the real request.
        if (req->method() == drogon::Options)
            return {};
        auto& limiter = Security::RateLimit::get();
        const auto& cfg = limiter.config();

        // The auth/account surface (login, register, refresh, password-reset,
        // token links) is auth-public, so the general public_paths skip below
        // would leave it unthrottled — the brute-force / mail-bomb hole. Route
        // those paths to the stricter per-IP tier FIRST, before the skip.
        const bool is_protected = Utils::Strings::path_is_public(cfg.protected_paths, req->path());
        if (!is_protected && Utils::Strings::path_is_public(cfg.public_paths, req->path()))
            return {};  // genuinely public infra/static (health, metrics, docs) — never limited

        Security::RateLimit::Decision d;
        int effective_limit;
        if (is_protected) {
            d = limiter.check_protected(Security::RateLimit::ip_identity(req, cfg));
            effective_limit = cfg.protected_requests;
        } else {
            d = limiter.check(Security::RateLimit::identity_for(req, cfg));
            effective_limit = cfg.requests;
        }
        // Stash limit metadata so the post-advice can emit X-RateLimit-* on
        // successful responses too, not only on 429.
        req->attributes()->insert("_rl_limit", effective_limit);
        req->attributes()->insert("_rl_remaining", d.remaining);
        if (d.allowed)
            return {};

        auto resp = ErrorResponse::too_many_requests(d.retry_after_sec);
        resp->addHeader("Retry-After", std::to_string(d.retry_after_sec));
        resp->addHeader("X-RateLimit-Limit", std::to_string(effective_limit));
        resp->addHeader("X-RateLimit-Remaining", "0");
        return resp;
    });

    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            // Only emit when the limiter actually ran (public paths skip it).
            // get<int>() returns 0 on a missing key rather than throwing, so
            // without this find() we'd stamp "X-RateLimit-Limit: 0" on every
            // public response.
            if (!req->attributes()->find("_rl_limit"))
                return;
            int limit = req->attributes()->get<int>("_rl_limit");
            int remaining = req->attributes()->get<int>("_rl_remaining");
            resp->addHeader("X-RateLimit-Limit", std::to_string(limit));
            resp->addHeader("X-RateLimit-Remaining", std::to_string(remaining));
        });
}

/**
 * @brief Double-submit-cookie CSRF guard (opt-in via security.csrf.enabled).
 * @details Enforces, for cookie-authenticated state-changing requests, that the
 *          CSRF cookie value is echoed in the configured header. The decision
 *          lives in Security::Csrf::passes() (unit-tested); this advice just
 *          feeds it the request's method/cookies/header. Off by default — the
 *          token cookie is emitted by set_session_cookies only when enabled.
 */
inline void register_csrf() {
    if (!Config::is_initialized())
        return;
    if (!Config::get().get<bool>("security.csrf.enabled", "SECURITY_CSRF_ENABLED", false))
        return;
    const std::string cookie_name =
        Config::get().get<std::string>("security.csrf.cookie_name", "SECURITY_CSRF_COOKIE", "csrf-token");
    const std::string header_name =
        Config::get().get<std::string>("security.csrf.header_name", "SECURITY_CSRF_HEADER", "X-CSRF-Token");
    std::string access_cookie = "__Host-access";
    if (Security::Auth::is_initialized())
        access_cookie = Security::Auth::get().config().cookies.access_name;

    drogon::app().registerSyncAdvice(
        [cookie_name, header_name, access_cookie](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
            const auto m = req->method();
            const bool unsafe = (m == drogon::Post || m == drogon::Put || m == drogon::Patch || m == drogon::Delete);
            if (Security::Csrf::passes(
                    unsafe, req->getCookie(access_cookie), req->getCookie(cookie_name), req->getHeader(header_name)))
                return {};
            return ErrorResponse::forbidden("csrf_failed", "CSRF token missing or invalid");
        });
}

inline void register_idempotency() {
    if (!Security::Idempotency::is_initialized())
        return;
    if (!Security::Idempotency::config().enabled)
        return;
    drogon::app().registerSyncAdvice(&Security::Idempotency::pre_handle);
    drogon::app().registerPostHandlingAdvice(&Security::Idempotency::post_handle);
}

/**
 * @brief Middleware that rejects POST/PUT/PATCH with non-JSON Content-Type.
 * @details Without this guard, json::parse(body) inside controllers throws on
 *          form-encoded or text bodies and surfaces as a 500. The spec answer
 *          is 415 Unsupported Media Type — easier to debug from the client
 *          side. Empty body (no Content-Type at all) is allowed: not every
 *          mutation carries a payload.
 *
 *          Recognized prefixes: application/json, application/*+json. Charset
 *          parameters are stripped before comparison.
 */
inline void register_content_type_check() {
    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        const auto m = req->method();
        if (m != drogon::Post && m != drogon::Put && m != drogon::Patch)
            return {};
        if (req->body().empty())
            return {};

        std::string ct = req->getHeader("Content-Type");
        // Strip ";charset=..." and any whitespace.
        auto semi = ct.find(';');
        if (semi != std::string::npos)
            ct.erase(semi);
        while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t'))
            ct.pop_back();
        size_t lead = 0;
        while (lead < ct.size() && (ct[lead] == ' ' || ct[lead] == '\t'))
            ++lead;
        ct.erase(0, lead);

        // application/json, or any structured-suffix JSON type
        // (e.g. application/merge-patch+json).
        const bool is_json = (ct == "application/json") || (ct.starts_with("application/") && ct.ends_with("+json"));
        if (is_json)
            return {};
        return ErrorResponse::unsupported_media_type("unsupported_media_type", "Content-Type must be application/json");
    });
}

inline std::vector<std::string> load_cors_origins() {
    if (!Config::is_initialized())
        return {};
    std::string csv = Config::get().get<std::string>("cors.allowed_origins", "CORS_ALLOWED_ORIGINS", "");
    if (csv.empty())
        return {};
    return Utils::Strings::split_csv_vec(csv);
}

inline void register_cors() {
    auto cors_origins = load_cors_origins();
    if (cors_origins.empty())
        return;

    drogon::app().registerSyncAdvice([cors_origins](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        if (req->method() != drogon::Options)
            return {};
        const auto& origin = req->getHeader("Origin");
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        if (!origin.empty() && std::find(cors_origins.begin(), cors_origins.end(), origin) != cors_origins.end()) {
            resp->addHeader("Access-Control-Allow-Origin", origin);
            resp->addHeader("Vary", "Origin");
        }
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        resp->addHeader("Access-Control-Max-Age", "600");
        return resp;
    });
    drogon::app().registerPostHandlingAdvice(
        [cors_origins](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            const auto& origin = req->getHeader("Origin");
            if (!origin.empty() && std::find(cors_origins.begin(), cors_origins.end(), origin) != cors_origins.end()) {
                resp->addHeader("Access-Control-Allow-Origin", origin);
                resp->addHeader("Vary", "Origin");
            }
        });
}

/**
 * @brief Stamp baseline security headers on every response.
 * @details API responses are JSON, so the CSP is locked all the way down
 *          (default-src 'none') — nothing should ever execute or embed from an
 *          API origin. The SPA's own HTML/CSP is set at the edge (nginx). HSTS
 *          is opt-in (security.hsts): it's only honoured over HTTPS, but gating
 *          it keeps it out of plain-http dev. set_if_absent never clobbers a
 *          header a handler deliberately set.
 */
inline void register_security_headers() {
    bool hsts = false;
    int hsts_max_age = 31536000;
    if (Config::is_initialized()) {
        hsts = Config::get().get<bool>("security.hsts", "SECURITY_HSTS", false);
        hsts_max_age = Config::get().get<int>("security.hsts_max_age", "SECURITY_HSTS_MAX_AGE", 31536000);
    }
    drogon::app().registerPostHandlingAdvice(
        [hsts, hsts_max_age](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
            auto set_if_absent = [&](const char* key, const std::string& value) {
                if (resp->getHeader(key).empty())
                    resp->addHeader(key, value);
            };
            set_if_absent("X-Content-Type-Options", "nosniff");
            set_if_absent("X-Frame-Options", "DENY");
            set_if_absent("Referrer-Policy", "no-referrer");
            set_if_absent("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
            if (hsts)
                set_if_absent("Strict-Transport-Security",
                              "max-age=" + std::to_string(hsts_max_age) + "; includeSubDomains");
        });
}

/**
 * @brief Holds the RuntimeContext token that marks the request's span as
 *        the ACTIVE one on this thread, so child spans (db.*) nest under
 *        it. Stored in request attributes (shared_ptr — attributes need
 *        copyable values); post-advice resets it on the same IO thread.
 */
struct OtelContextToken {
    opentelemetry::nostd::unique_ptr<opentelemetry::context::Token> token;
};

inline constexpr const char* kOtelTokenAttr = "_otel_ctx_token";

inline void register_tracing_pre() {
    drogon::app().registerPreHandlingAdvice([](const drogon::HttpRequestPtr& req) {
        req->attributes()->insert("_req_start", std::chrono::steady_clock::now());
        // Defensive: start from a clean ambient traceparent in case a prior
        // request on this IO thread didn't reach finish_span (e.g. post-advice
        // threw). We set the correct value at the end of this advice.
        Observability::Trace::clear_current_traceparent();

        // W3C Trace Context: parse incoming `traceparent` or generate a new
        // trace ID so downstream advice + logs can reference the same ID.
        const auto& incoming_tp = req->getHeader("traceparent");
        const auto parsed = Observability::Trace::parse_traceparent(std::string_view(incoming_tp));
        const auto tctx = parsed ? *parsed : Observability::Trace::generate_context();

        // Compute the normalized route ONCE and stash it — the access-log
        // post-advice reuses it instead of running the segment scan a second
        // time. It also redacts UUIDs and account tokens, so it's what we log
        // (the raw path would leak password-reset tokens into the log).
        const std::string route = normalize_path_for_metrics(req->path());
        req->attributes()->insert("_norm_path", route);

        // Defaults from the string context (used when OTel tracing is disabled).
        // When it's on we OVERWRITE these with the REAL server span's ids below,
        // so logs, the response traceparent, and the context handed to jobs all
        // reference THIS span — not the caller's span id (the old bug) or a
        // phantom generated id.
        std::string trace_id = tctx.trace_id;
        std::string span_id = tctx.parent_id;

        if (Observability::is_initialized()) {
            auto tracer = Observability::get().tracer().get_tracer("http");
            opentelemetry::trace::StartSpanOptions opts;
            opts.kind = opentelemetry::trace::SpanKind::kServer;
            // Join the caller's distributed trace when a valid traceparent
            // arrived — our span becomes a child of the upstream client span
            // instead of an unrelated root.
            if (parsed) {
                if (auto remote = detail::to_remote_span_context(*parsed))
                    opts.parent = *remote;
            }
            // Normalized operation name ("GET /api/jobs/:id"): raw paths would
            // mint a new Jaeger operation per UUID and leak account tokens.
            auto span = tracer->StartSpan(
                std::string(req->getMethodString()) + " " + route,
                {{"http.method", std::string(req->getMethodString())}, {"http.route", route}, {"http.target", route}},
                opts);
            // Read the real span context: its trace_id continues the upstream
            // trace (or is a fresh root) and its span_id is what child spans
            // (db.*, and the worker's job span) must parent off. Only when it's
            // valid though — with tracing disabled the provider hands back a
            // no-op span whose context is all-zero; keep the string context
            // (incoming/generated) for x-request-id + propagation in that case.
            const auto sctx = span->GetContext();
            if (sctx.IsValid()) {
                char tid[32];
                sctx.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(tid, 32));
                trace_id.assign(tid, 32);
                char sid[16];
                sctx.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>(sid, 16));
                span_id.assign(sid, 16);
            }
            req->attributes()->insert("_trace_span", span);

            // Activate the span on this thread so db.* spans pick it up as
            // parent. Handlers here are synchronous (the callback fires
            // inside the handler call on the same IO thread), and the
            // post-advice — same thread — releases the token. If you add an
            // ASYNC handler, detach/re-attach around the suspension points.
            auto holder = std::make_shared<OtelContextToken>();
            // SetSpan wants a non-const lvalue Context in this OTel version.
            auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
            holder->token =
                opentelemetry::context::RuntimeContext::Attach(opentelemetry::trace::SetSpan(current_ctx, span));
            req->attributes()->insert(kOtelTokenAttr, holder);
        }

        // Publish the resolved ids (real span when OTel is up, string context
        // otherwise) for the access log, the response traceparent header, and
        // the ambient traceparent that Jobs::submit hands to the worker.
        req->attributes()->insert(Observability::Trace::kTraceIdAttr, trace_id);
        req->attributes()->insert(Observability::Trace::kSpanIdAttr, span_id);
        req->attributes()->insert(Observability::Trace::kTraceFlagsAttr, tctx.flags);
        // Ambient on this IO thread for the synchronous handler; cleared in
        // finish_span (post-advice, same thread).
        Observability::Trace::set_current_traceparent({trace_id, span_id, tctx.flags});
    });
}

namespace access_log_detail {

struct Timing {
    double duration_ms;
    double duration_seconds;
};

inline Timing measure(const drogon::HttpRequestPtr& req) {
    // _req_start is only set by the tracing pre-advice; on short-circuit paths
    // (auth/415/429 reject before it runs) it's absent. get<T>() returns a
    // default-constructed time_point (epoch) there, NOT a throw — so we'd
    // report uptime-as-latency. Guard with find().
    if (!req->attributes()->find("_req_start"))
        return {0, 0};
    try {
        auto start = req->attributes()->get<std::chrono::steady_clock::time_point>("_req_start");
        auto elapsed = std::chrono::steady_clock::now() - start;
        return {std::chrono::duration<double, std::milli>(elapsed).count(),
                std::chrono::duration<double>(elapsed).count()};
    } catch (...) {
        return {0, 0};
    }
}

inline std::string emit_trace_headers(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
    std::string trace_id;
    try {
        trace_id = req->attributes()->get<std::string>(Observability::Trace::kTraceIdAttr);
    } catch (...) {
        return {};
    }
    if (trace_id.empty())
        return {};
    resp->addHeader("X-Request-Id", trace_id);
    try {
        std::string span_id = req->attributes()->get<std::string>(Observability::Trace::kSpanIdAttr);
        std::string flags = req->attributes()->get<std::string>(Observability::Trace::kTraceFlagsAttr);
        resp->addHeader("traceparent", Observability::Trace::format_traceparent({trace_id, span_id, flags}));
    } catch (...) {}
    return trace_id;
}

// Single bucket for all unmatched (404) routes — see record_metrics.
inline constexpr const char* kUnmatchedMetricPath = "<unmatched>";

inline void record_metrics(const std::string& method,
                           const std::string& norm_path,
                           int status,
                           double duration_seconds) {
    // Cap label cardinality: an unmatched route (404) carries an arbitrary,
    // attacker-controllable path that normalize_path_for_metrics can't bucket
    // (no :id/:token segments) — emitting it verbatim lets `/api/<random>`
    // mint unbounded {path} series → registry/TSDB OOM. Collapse all 404s to a
    // single constant label.
    const std::string& path = (status == 404) ? kUnmatchedMetricPath : norm_path;
    if (http_requests_family) {
        try {
            http_requests_family->Add({{"method", method}, {"path", path}, {"status", std::to_string(status)}})
                .Increment();
        } catch (...) {}
    }
    if (http_duration_family && duration_seconds > 0) {
        try {
            http_duration_family->Add({{"method", method}, {"path", path}}, HTTP_DURATION_BUCKETS)
                .Observe(duration_seconds);
        } catch (...) {}
    }
}

inline void finish_span(const drogon::HttpRequestPtr& req, int status) {
    // Clear the ambient traceparent set in pre-advice (same IO thread) so it
    // can't leak into the next request served by this thread.
    Observability::Trace::clear_current_traceparent();
    // Detach the RuntimeContext token FIRST (post-advice runs on the same
    // IO thread that attached it) so the thread's active span is restored
    // before we close ours.
    try {
        if (req->attributes()->find(middleware::kOtelTokenAttr)) {
            auto holder =
                req->attributes()->get<std::shared_ptr<middleware::OtelContextToken>>(middleware::kOtelTokenAttr);
            if (holder)
                holder->token.reset();
        }
    } catch (...) {}
    // _trace_span is only set when Observability was up at pre-advice time;
    // get<T>() on a missing key returns a default (null) shared_ptr, not a
    // throw, so guard with find() + null-check before dereferencing.
    if (!req->attributes()->find("_trace_span"))
        return;
    try {
        auto span = req->attributes()->get<detail::TraceSpan>("_trace_span");
        if (!span)
            return;
        span->SetAttribute("http.status_code", status);
        span->SetStatus(status >= 500 ? opentelemetry::trace::StatusCode::kError
                                      : opentelemetry::trace::StatusCode::kOk);
        span->End();
    } catch (...) {}
}

}  // namespace access_log_detail

inline void register_access_log_post() {
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            const auto timing = access_log_detail::measure(req);
            const std::string method = std::string(req->getMethodString());
            // Reuse the route the pre-advice computed; fall back to a fresh
            // compute on the short-circuit paths (auth/415/429) where the
            // pre-advice never ran. Never log req->path() raw — it carries
            // account tokens.
            std::string norm_path;
            if (req->attributes()->find("_norm_path"))
                norm_path = req->attributes()->get<std::string>("_norm_path");
            else
                norm_path = normalize_path_for_metrics(req->path());
            const int status = static_cast<int>(resp->statusCode());

            const std::string trace_id = access_log_detail::emit_trace_headers(req, resp);
            spdlog::info("{} {} {} {:.3f}ms tid={}", method, norm_path, status, timing.duration_ms, trace_id);
            access_log_detail::record_metrics(method, norm_path, status, timing.duration_seconds);
            access_log_detail::finish_span(req, status);
        });
}

}  // namespace middleware

/**
 * @brief Register /api/v1/docs (Swagger UI) and /api/v1/openapi.yaml if
 *        `docs.enabled` is true. Off by default — intended for dev and
 *        internal deployments, never production. The Swagger UI HTML is
 *        served inline (tiny snippet pointing at the unpkg CDN), and the
 *        YAML is streamed from the path configured in `docs.openapi_path`.
 */
inline void register_docs_endpoints() {
    if (!Config::is_initialized())
        return;
    if (!Config::get().get<bool>("docs.enabled", "DOCS_ENABLED", false))
        return;

    const std::string yaml_path =
        Config::get().get<std::string>("docs.openapi_path", "DOCS_OPENAPI_PATH", "docs/openapi.yaml");
    spdlog::info("Swagger UI enabled — mounting /api/v1/docs (spec from {})", yaml_path);

    drogon::app().registerHandler(
        "/api/v1/openapi.yaml",
        [yaml_path](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            std::ifstream f(yaml_path);
            if (!f.good()) {
                cb(ErrorResponse::not_found("openapi_spec"));
                return;
            }
            std::stringstream buf;
            buf << f.rdbuf();
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString("application/yaml; charset=utf-8");
            resp->setBody(buf.str());
            cb(resp);
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/docs",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            static const std::string kSwaggerUiHtml = R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><title>API docs</title>
<link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
</head><body><div id="swagger-ui"></div>
<script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
<script>window.onload = () => { SwaggerUIBundle({
  url: '/api/v1/openapi.yaml', dom_id: '#swagger-ui', deepLinking: true,
  presets: [SwaggerUIBundle.presets.apis, SwaggerUIBundle.SwaggerUIStandalonePreset]
}); };</script></body></html>)";
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString("text/html; charset=utf-8");
            resp->setBody(kSwaggerUiHtml);
            cb(resp);
        },
        {drogon::Get});
}

}  // namespace Api
