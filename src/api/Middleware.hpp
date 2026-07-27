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
#include <cctype>
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

namespace detail {

/// Baseline security-header settings, resolved once at registration time by
/// register_security_headers() and read by BOTH the post-handling advice and
/// the sync-advice short-circuit path (which never reaches that advice).
inline bool hsts_enabled = false;
inline int hsts_max_age_sec = 31536000;

/// @see register_security_headers for why each header is here. set_if_absent
/// never clobbers a header a handler (or the CORS advice) deliberately set.
inline void apply_security_headers(const drogon::HttpResponsePtr& resp) {
    auto set_if_absent = [&](const char* key, const std::string& value) {
        if (resp->getHeader(key).empty())
            resp->addHeader(key, value);
    };
    set_if_absent("X-Content-Type-Options", "nosniff");
    set_if_absent("X-Frame-Options", "DENY");
    set_if_absent("Referrer-Policy", "no-referrer");
    set_if_absent("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    if (hsts_enabled)
        set_if_absent("Strict-Transport-Security",
                      "max-age=" + std::to_string(hsts_max_age_sec) + "; includeSubDomains");
}

/// Echo the limiter's budget for this request. Shared by the rate-limit
/// post-handling advice and the short-circuit path, so the 429 the limiter
/// itself returns carries the same headers as an allowed response.
inline void apply_rate_limit_headers(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
    // Only emit when the limiter actually ran (public paths skip it).
    // get<int>() returns 0 on a missing key rather than throwing, so without
    // this find() we'd stamp "X-RateLimit-Limit: 0" on every public response.
    if (!req->attributes()->find("_rl_limit"))
        return;
    resp->addHeader("X-RateLimit-Limit", std::to_string(req->attributes()->get<int>("_rl_limit")));
    resp->addHeader("X-RateLimit-Remaining", std::to_string(req->attributes()->get<int>("_rl_remaining")));
}

/**
 * @brief Mint (once per request) the start timestamp, the normalized route and
 *        the request/trace ids, and return the resolved string trace context.
 * @details Registered as the FIRST sync advice (register_request_id) so that
 *          responses produced by a later sync advice — which skip the entire
 *          pre/post-handling chain — still have an id to stamp. Idempotent:
 *          the tracing pre-advice calls it again and reuses what's there
 *          instead of generating a second, conflicting id.
 */
inline Observability::Trace::TraceContext ensure_request_ids(const drogon::HttpRequestPtr& req) {
    if (req->attributes()->find(Observability::Trace::kTraceIdAttr)) {
        return {req->attributes()->get<std::string>(Observability::Trace::kTraceIdAttr),
                req->attributes()->get<std::string>(Observability::Trace::kSpanIdAttr),
                req->attributes()->get<std::string>(Observability::Trace::kTraceFlagsAttr)};
    }
    req->attributes()->insert("_req_start", std::chrono::steady_clock::now());
    // W3C Trace Context: continue the caller's `traceparent` or generate a new
    // trace ID so downstream advice + logs can reference the same ID.
    const auto parsed = Observability::Trace::parse_traceparent(std::string_view(req->getHeader("traceparent")));
    const auto tctx = parsed ? *parsed : Observability::Trace::generate_context();
    // Compute the normalized route ONCE — the access log reuses it instead of
    // running the segment scan again. It also redacts UUIDs and account tokens,
    // so it's what we log (the raw path would leak password-reset tokens).
    req->attributes()->insert("_norm_path", normalize_path_for_metrics(req->path()));
    req->attributes()->insert(Observability::Trace::kTraceIdAttr, tctx.trace_id);
    req->attributes()->insert(Observability::Trace::kSpanIdAttr, tctx.parent_id);
    req->attributes()->insert(Observability::Trace::kTraceFlagsAttr, tctx.flags);
    return tctx;
}

}  // namespace detail

/**
 * @brief Terminate a request that a sync advice is short-circuiting.
 * @details Drogon's HttpServer::passSyncAdvices() writes the response and
 *          returns false, so NOTHING else runs for it — no pre-handling
 *          advice, and no post-handling chain: no X-Request-Id, no
 *          traceparent, no baseline security headers, no access-log line, no
 *          metric sample. Every sync advice therefore returns THROUGH this
 *          helper, which replays what the post-handling chain would have done.
 *          Defined below (it needs access_log_detail); declared here because
 *          the advice lambdas above call it.
 */
inline drogon::HttpResponsePtr short_circuit(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp);

/**
 * @brief First sync advice in the chain: mint the request/trace ids.
 * @details Never returns a response — it exists purely so that the advices
 *          registered after it (content-type, auth, csrf, rate limit,
 *          idempotency, cors) have ids to stamp on a short-circuited response.
 */
inline void register_request_id() {
    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        detail::ensure_request_ids(req);
        return {};
    });
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
            return short_circuit(req, resp);
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
        // X-RateLimit-* comes from the stashed metadata above via
        // short_circuit() — the same helper the post-handling advice uses, so
        // a 429 and an allowed response carry identical budget headers.
        return short_circuit(req, resp);
    });

    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            detail::apply_rate_limit_headers(req, resp);
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
            return short_circuit(req, ErrorResponse::forbidden("csrf_failed", "CSRF token missing or invalid"));
        });
}

inline void register_idempotency() {
    if (!Security::Idempotency::is_initialized())
        return;
    if (!Security::Idempotency::config().enabled)
        return;
    // Wrapped rather than registered directly: a replay / conflict / in-progress
    // response short-circuits the chain, so it needs short_circuit() to pick up
    // its request id, security headers, log line and metric sample.
    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        auto resp = Security::Idempotency::pre_handle(req);
        if (!resp)
            return {};
        return short_circuit(req, resp);
    });
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
 *          Recognized prefixes: application/json and any application/...+json. Charset
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
        // Media types and subtypes are case-INSENSITIVE (RFC 9110 §8.3.1), so
        // `Application/JSON` and `Multipart/Form-Data` are legal and must not
        // 415. Fold to ASCII lowercase before comparing.
        std::transform(ct.begin(), ct.end(), ct.begin(), [](unsigned char c) { return std::tolower(c); });

        // application/json, or any structured-suffix JSON type
        // (e.g. application/merge-patch+json).
        const bool is_json = (ct == "application/json") || (ct.starts_with("application/") && ct.ends_with("+json"));
        // multipart/form-data is the legitimate non-JSON body: file uploads
        // (POST /api/v1/admin/uploads). Without this exemption the gate 415s
        // EVERY upload before UploadController — which does its own strict
        // validation (admin gate + magic-byte sniff + size cap) — ever runs.
        const bool is_multipart = ct.starts_with("multipart/form-data");
        if (is_json || is_multipart)
            return {};
        constexpr const char* kMsg = "Content-Type must be application/json or multipart/form-data";
        return short_circuit(req, ErrorResponse::unsupported_media_type("unsupported_media_type", kMsg));
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
        return short_circuit(req, resp);
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
 *
 *          The header set itself lives in detail::apply_security_headers so the
 *          sync-advice short-circuit path (which never reaches a post-handling
 *          advice) stamps exactly the same headers.
 */
inline void register_security_headers() {
    if (Config::is_initialized()) {
        detail::hsts_enabled = Config::get().get<bool>("security.hsts", "SECURITY_HSTS", false);
        detail::hsts_max_age_sec = Config::get().get<int>("security.hsts_max_age", "SECURITY_HSTS_MAX_AGE", 31536000);
    }
    drogon::app().registerPostHandlingAdvice([](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
        detail::apply_security_headers(resp);
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
        // Defensive: start from a clean ambient traceparent in case a prior
        // request on this IO thread didn't reach finish_span (e.g. post-advice
        // threw). We set the correct value at the end of this advice.
        Observability::Trace::clear_current_traceparent();

        // _req_start, the normalized route and the W3C ids were already minted
        // by the request-id sync advice (registered FIRST, so it ran for this
        // request). Reuse them — regenerating here would hand the access log a
        // different id than the one a short-circuiting advice would have
        // stamped, and would restart the latency clock.
        const auto tctx = detail::ensure_request_ids(req);
        const std::string route = req->attributes()->get<std::string>("_norm_path");
        const auto& incoming_tp = req->getHeader("traceparent");
        const auto parsed = Observability::Trace::parse_traceparent(std::string_view(incoming_tp));

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
    // _req_start is stamped by detail::ensure_request_ids (the first sync
    // advice), so it's present on the short-circuit paths too. It can still be
    // absent for a response produced outside the advice chain; get<T>() returns
    // a default-constructed time_point (epoch) there, NOT a throw — so we'd
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

// Declared above the advices that call it; see the doc comment there.
inline drogon::HttpResponsePtr short_circuit(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
    if (!resp)
        return resp;
    detail::ensure_request_ids(req);  // no-op when register_request_id() ran
    const auto timing = access_log_detail::measure(req);
    const std::string method = std::string(req->getMethodString());
    std::string norm_path;
    if (req->attributes()->find("_norm_path"))
        norm_path = req->attributes()->get<std::string>("_norm_path");
    else
        norm_path = normalize_path_for_metrics(req->path());
    const int status = static_cast<int>(resp->statusCode());

    // Same order the post-handling chain would have applied them in:
    // trace headers → rate-limit budget → baseline security headers, then
    // exactly one access-log line and one metric sample.
    const std::string trace_id = access_log_detail::emit_trace_headers(req, resp);
    detail::apply_rate_limit_headers(req, resp);
    detail::apply_security_headers(resp);
    spdlog::info("{} {} {} {:.3f}ms tid={}", method, norm_path, status, timing.duration_ms, trace_id);
    // Cardinality guard — the SAME reason record_metrics collapses 404s, and it
    // applies here even harder. A sync advice runs BEFORE routing, so norm_path
    // is arbitrary caller input that no route ever matched, and the status is
    // 401/415/429/204 rather than the 404 record_metrics buckets. Labelling the
    // sample with it lets an UNAUTHENTICATED loop over /x/<random> (401 from the
    // auth advice, or 204 from the OPTIONS preflight, neither of which the
    // limiter sees) mint one {path} series per URL → registry/TSDB OOM. The
    // access-log line above still carries the real normalized route.
    access_log_detail::record_metrics(method, access_log_detail::kUnmatchedMetricPath, status, timing.duration_seconds);
    // No server span exists on this path (the tracing pre-advice never ran);
    // finish_span still clears the ambient traceparent for this IO thread.
    access_log_detail::finish_span(req, status);
    return resp;
}

inline void register_access_log_post() {
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            const auto timing = access_log_detail::measure(req);
            const std::string method = std::string(req->getMethodString());
            // Reuse the route the request-id advice computed; fall back to a
            // fresh compute for a response minted outside the advice chain.
            // Never log req->path() raw — it carries account tokens.
            // (Short-circuited responses never reach here at all: they are
            // logged once by middleware::short_circuit instead.)
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
