/**
 * @file Idempotency.hpp
 * @brief Idempotency-Key middleware for mutating HTTP requests.
 * @details When a client sends `Idempotency-Key: <key>` on POST/PUT/PATCH/DELETE,
 *          the first request's response is stored in Redis keyed by
 *          (method, path, key). A subsequent request with the same key:
 *          - replays the cached response if the body hash matches;
 *          - returns 422 if the body hash differs (signals client bug).
 *          If Redis is unavailable we fail open (request proceeds normally) —
 *          the alternative (reject all mutating requests when cache is down)
 *          is worse than a rare double-processing.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "security/Auth.hpp"
#include "utils/Config.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"

namespace Security::Idempotency {

using json = nlohmann::json;

struct Config {
    bool enabled = false;
    long ttl_sec = 86400;                    // 24h
    size_t max_body_bytes = 1024 * 1024;     // 1 MiB — reject larger REQUEST bodies
    size_t max_response_bytes = 256 * 1024;  // 256 KiB — skip caching oversized responses
    // Lock TTL for the in-flight marker (SET NX). Bounds how long a crashed or
    // stuck handler can block a retry from another client with the same key.
    // Should comfortably exceed p99 handler latency.
    int lock_ttl_sec = 30;
};

namespace detail {

inline bool is_mutating(drogon::HttpMethod m) {
    return m == drogon::Post || m == drogon::Put || m == drogon::Delete || m == drogon::Patch;
}

// Namespaced by the authenticated principal (or "anon") so two users sending
// the same Idempotency-Key can't read back each other's cached responses —
// idempotency keys aren't secrets, so a shared key must not cross users.
inline std::string cache_key(const std::string& principal,
                             const std::string& method,
                             const std::string& path,
                             const std::string& idem_key) {
    // Hash the path: it can carry single-use tokens (e.g. an idempotent retry of
    // a token-bearing route) which must not land verbatim in Redis key names
    // (visible via KEYS / RDB dumps / SLOWLOG) — everywhere else they're redacted.
    return "idemp:" + principal + ":" + method + ":" + Utils::Crypto::sha256_hex(path) + ":" + idem_key;
}

inline std::string lock_key(const std::string& ck) {
    return ck + ":lock";
}

}  // namespace detail

inline std::unique_ptr<Config> global_cfg = nullptr;

inline void initialize() {
    if (global_cfg != nullptr) {
        // Warned no-op on repeated initialize — same convention as Auth and
        // RateLimit. Call shutdown() first to reconfigure.
        spdlog::warn("Idempotency::initialize called twice — keeping existing config");
        return;
    }
    auto cfg = std::make_unique<Config>();
    if (::Config::is_initialized()) {
        auto& c = ::Config::get();
        cfg->enabled = c.get<bool>("idempotency.enabled", "IDEMPOTENCY_ENABLED", false);
        cfg->ttl_sec = c.get<int>("idempotency.ttl_sec", "IDEMPOTENCY_TTL_SEC", 86400);
        int mb = c.get<int>("idempotency.max_body_kb", "IDEMPOTENCY_MAX_BODY_KB", 1024);
        cfg->max_body_bytes = static_cast<size_t>(std::max(1, mb)) * 1024;
        int rmb = c.get<int>("idempotency.max_response_kb", "IDEMPOTENCY_MAX_RESPONSE_KB", 256);
        cfg->max_response_bytes = static_cast<size_t>(std::max(1, rmb)) * 1024;
        cfg->lock_ttl_sec = c.get<int>("idempotency.lock_ttl_sec", "IDEMPOTENCY_LOCK_TTL_SEC", 30);
        if (cfg->lock_ttl_sec < 1)
            cfg->lock_ttl_sec = 1;
    }
    global_cfg = std::move(cfg);
    spdlog::info(
        "Idempotency middleware initialized: enabled={} ttl={}s lock_ttl={}s max_body={}KiB max_response={}KiB",
        global_cfg->enabled,
        global_cfg->ttl_sec,
        global_cfg->lock_ttl_sec,
        global_cfg->max_body_bytes / 1024,
        global_cfg->max_response_bytes / 1024);
}

inline bool is_initialized() {
    return global_cfg != nullptr;
}

inline const Config& config() {
    if (global_cfg == nullptr)
        throw std::runtime_error("Idempotency not initialized");
    return *global_cfg;
}

inline void shutdown() {
    global_cfg.reset();
}

// ---------------------------------------------------------------------------
// Middleware attribute keys (shared between pre- and post-handlers).
// ---------------------------------------------------------------------------

inline constexpr const char* kKeyAttr = "_idemp_cache_key";
inline constexpr const char* kHashAttr = "_idemp_body_hash";
inline constexpr const char* kLockKeyAttr = "_idemp_lock_key";

/**
 * @brief Pre-handling check. Returns a response to short-circuit the handler
 *        (either a replay of a prior response, or a 422 on conflict), or
 *        empty to let the request proceed with the key stashed in attributes.
 */
namespace detail {

inline drogon::HttpResponsePtr make_conflict_response() {
    return ErrorResponse::unprocessable("idempotency_key_conflict", "body hash mismatch for previously-seen key");
}

// 409 — another request with the same Idempotency-Key is being processed
// right now. Per the IETF idempotency draft, the client should retry after
// a short backoff; we surface the lock TTL via Retry-After so naive retry
// loops don't immediately hammer us again.
inline drogon::HttpResponsePtr make_in_progress_response(int retry_after_sec) {
    auto resp = ErrorResponse::conflict("idempotency_key_in_progress",
                                        "another request with this Idempotency-Key is in progress");
    resp->addHeader("Retry-After", std::to_string(std::max(1, retry_after_sec)));
    return resp;
}

inline drogon::HttpResponsePtr make_replay_response(const json& stored) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(stored.value("status", 200)));

    // Rebuild the original content type + headers so clients that depend on
    // Location, Content-Type, or custom X-* headers see the same shape they
    // would have seen on the first call. Drogon stores Content-Type both as
    // a header and as an enum; setting the header is sufficient and it won't
    // get overridden because we're not calling setContentTypeCode.
    const std::string content_type = stored.value("content_type", std::string("application/json"));
    resp->setContentTypeString(content_type);

    if (stored.contains("headers") && stored["headers"].is_object()) {
        for (auto it = stored["headers"].begin(); it != stored["headers"].end(); ++it) {
            if (it.value().is_string()) {
                resp->addHeader(it.key(), it.value().get<std::string>());
            }
        }
    }

    resp->addHeader("X-Idempotent-Replayed", "true");
    resp->setBody(stored.value("body", std::string{}));
    return resp;
}

// Read a stored idempotency entry. Returns nullopt on miss, corrupt payload,
// or Redis error — callers fail open in all these cases. A corrupt entry is
// deleted so the next attempt stores fresh state.
inline std::optional<json> read_stored_entry(const std::string& ck) {
    try {
        auto cached = Cache::get().get(ck);
        if (!cached)
            return std::nullopt;
        try {
            return json::parse(*cached);
        } catch (...) {
            spdlog::warn("idempotency: corrupted entry at {}, dropping", ck);
            Cache::get().del(ck);
            return std::nullopt;
        }
    } catch (const std::exception& e) {
        spdlog::warn("idempotency: cache read failed, failing open: {}", e.what());
        return std::nullopt;
    }
}

}  // namespace detail

inline drogon::HttpResponsePtr pre_handle(const drogon::HttpRequestPtr& req) {
    if (!is_initialized() || !config().enabled)
        return {};
    if (!detail::is_mutating(req->method()))
        return {};

    const auto& key = req->getHeader("Idempotency-Key");
    if (key.empty())
        return {};
    if (!Cache::is_initialized()) {
        spdlog::warn("idempotency: cache not initialized, failing open");
        return {};
    }

    std::string body(req->body());
    if (body.size() > config().max_body_bytes) {
        return ErrorResponse::payload_too_large("body_too_large_for_idempotency");
    }

    const std::string body_hash = Utils::Crypto::sha256_hex(body);
    // Scope the entry to the caller: the auth middleware has already stamped
    // the principal by now (idempotency runs after auth). When auth is off
    // every caller is "anon", so fold the peer IP into the scope — otherwise
    // two different real users sharing an Idempotency-Key would cross-replay
    // each other's cached responses (a data leak under the AUTH_MODE=none
    // default). With auth on, the principal subject is unique and used as-is.
    std::string principal;
    if (auto p = Security::Auth::principal_of(req); p && !p->subject.empty())
        principal = p->subject;
    else
        principal = "anon:" + req->getPeerAddr().toIp();
    const std::string ck = detail::cache_key(principal, std::string(req->getMethodString()), req->path(), key);

    if (auto stored = detail::read_stored_entry(ck)) {
        if (stored->value("req_hash", "") != body_hash) {
            return detail::make_conflict_response();
        }
        return detail::make_replay_response(*stored);
    }

    // Cache miss. Take a SET NX lock to serialize concurrent first-time
    // requests with the same key — otherwise two retries that race past
    // the read above would both reach the handler and double-process.
    // Lock value is the body hash so post_handle can verify ownership
    // before the cleanup DEL (defense against stale-lock takeover).
    const std::string lk = detail::lock_key(ck);
    bool got_lock = false;
    try {
        got_lock = Cache::get().set_nx(lk, body_hash, std::chrono::seconds(config().lock_ttl_sec));
    } catch (const std::exception& e) {
        spdlog::warn("idempotency: lock acquire failed, failing open: {}", e.what());
        // Fall through with no lock held — same fail-open posture as a
        // cache read failure. Better than rejecting valid traffic.
    }

    if (!got_lock) {
        // Another request holds the lock. Re-read once: the holder may have
        // just finished and written the response, in which case we can
        // replay instead of rejecting. This race is narrow but worth
        // handling — turns a 409 into a 200 for a very common retry pattern.
        if (auto stored = detail::read_stored_entry(ck)) {
            if (stored->value("req_hash", "") != body_hash) {
                return detail::make_conflict_response();
            }
            return detail::make_replay_response(*stored);
        }
        return detail::make_in_progress_response(config().lock_ttl_sec);
    }

    // Lock held — stash metadata so post_handle can cache the response and
    // release the lock.
    req->attributes()->insert(kKeyAttr, ck);
    req->attributes()->insert(kHashAttr, body_hash);
    req->attributes()->insert(kLockKeyAttr, lk);
    return {};
}

/**
 * @brief Post-handling hook. Stores the response (body + status) in Redis
 *        so a future request with the same Idempotency-Key can replay it.
 *        Only 2xx responses are cached — errors may be transient.
 */
inline void post_handle(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
    if (!is_initialized() || !config().enabled)
        return;
    if (!Cache::is_initialized())
        return;
    // pre_handle only stamps kKeyAttr when the request carried an
    // Idempotency-Key. Drogon's attributes()->get<T>() returns a
    // default-constructed T on a missing key (it does NOT throw on current
    // versions — the CONVENTIONS gotchas), so we MUST check find() first;
    // otherwise every non-idempotent 2xx would fall through with ck=="" and
    // get cached under the empty key. (See Auth::principal_of for the pattern.)
    if (!req->attributes()->find(kKeyAttr))
        return;  // This request wasn't under idempotent tracking.
    const std::string ck = req->attributes()->get<std::string>(kKeyAttr);
    const std::string body_hash = req->attributes()->get<std::string>(kHashAttr);
    std::string lk;
    if (req->attributes()->find(kLockKeyAttr)) {
        lk = req->attributes()->get<std::string>(kLockKeyAttr);
    }
    // else: pre_handle didn't take a lock (acquire errored, fail-open path) —
    // we still cache the result on success below, just skip the lock release.

    // Whatever happens, drop the in-flight lock so the next retry can proceed.
    // Done in a finally-style guard — even if the cache write below throws.
    struct LockGuard {
        std::string key;
        std::string owner;  // the body_hash we wrote as the lock value in pre_handle
        ~LockGuard() {
            if (key.empty() || !Cache::is_initialized())
                return;
            try {
                // Compare-and-delete: only release the lock if we still own it.
                // If this handler outlived lock_ttl_sec another request may have
                // re-acquired the lock; an unconditional DEL would free *their*
                // lock and let a third request run concurrently — the exact
                // double-processing the lock exists to prevent. Lua makes the
                // GET==owner check and the DEL atomic.
                static constexpr const char* kCad =
                    "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) else return 0 end";
                Cache::get().get_client().eval<long long>(kCad, {key}, {owner});
            } catch (const std::exception& e) {
                // Fail-open like the rest of the cache layer, but don't go
                // silent: a stuck lock blocks retries until its TTL expires.
                spdlog::warn("Idempotency: failed to release in-flight lock {}: {}", key, e.what());
            } catch (...) {
                spdlog::warn("Idempotency: failed to release in-flight lock {} (unknown error)", key);
            }
        }
    } guard{lk, body_hash};

    const int status = static_cast<int>(resp->statusCode());
    if (status < 200 || status >= 300)
        return;
    // Refuse to cache oversized responses — Redis memory is precious and
    // a 10MiB file download with 24h TTL would eat it. Drop the entry
    // silently (with a warn log); the client will re-execute on retry.
    std::string body(resp->getBody());
    if (body.size() > config().max_response_bytes) {
        spdlog::warn("idempotency: response at {} is {}KiB, exceeds cap {}KiB — not caching",
                     ck,
                     body.size() / 1024,
                     config().max_response_bytes / 1024);
        return;
    }
    try {
        // Preserve caller-visible headers so the replay is indistinguishable
        // from the original response. Skip hop-by-hop / auto-generated ones
        // and store Content-Type separately so the replay uses it directly.
        json headers = json::object();
        static const std::unordered_set<std::string> kSkip = {
            "content-length", "date", "server", "connection", "transfer-encoding", "content-type"};
        for (const auto& h : resp->headers()) {
            std::string k = h.first;
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
            if (kSkip.count(k) > 0)
                continue;
            headers[h.first] = h.second;
        }
        // Drogon doesn't expose the rendered Content-Type as a string
        // after setContentTypeCode — easiest reliable source is the
        // Content-Type header that always gets emitted on the wire.
        std::string ct = std::string(resp->getHeader("content-type"));
        if (ct.empty())
            ct = "application/json";
        json entry = {{"req_hash", body_hash},
                      {"status", status},
                      {"content_type", ct},
                      {"headers", std::move(headers)},
                      {"body", std::move(body)}};
        Cache::get().set(ck, entry.dump(), config().ttl_sec);
    } catch (const std::exception& e) {
        spdlog::warn("idempotency: cache write failed: {}", e.what());
    }
}

}  // namespace Security::Idempotency
