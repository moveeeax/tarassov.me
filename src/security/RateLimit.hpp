/**
 * @file RateLimit.hpp
 * @brief Redis-backed SLIDING-window request rate limiter.
 * @details One atomic Lua script per check: ZREMRANGEBYSCORE + ZCARD + ZADD
 *          over a per-identity sorted set — a true trailing window, immune to
 *          the classic 2x burst at fixed-window boundaries.
 *          Fail-open: if Redis is unavailable, requests pass through with a
 *          warning log — we prefer availability over strict enforcement for a
 *          template. Production deployments that need hard caps should set
 *          fail_open=false (rejects with 503 instead).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "security/Auth.hpp"
#include "utils/Config.hpp"
#include "utils/Strings.hpp"
#include "utils/Time.hpp"

namespace Security::RateLimit {

using json = nlohmann::json;

// Scope controls how a request is bucketed:
//   Ip        — always by client IP.
//   IpOrUser  — by authenticated user if one is present, else by IP.
// An earlier draft exposed a third "user-only" scope, but in practice that
// lumped every anonymous caller into a single bucket — hostile for any route
// that allows unauthenticated traffic. We fold it into IpOrUser.
enum class Scope { Ip, IpOrUser };

inline Scope parse_scope(const std::string& s) {
    if (s == "ip")
        return Scope::Ip;
    // "user", "ip_or_user", anything else → IpOrUser.
    return Scope::IpOrUser;
}

struct Config {
    bool enabled = false;
    int requests = 60;    // max per window
    int window_sec = 60;  // window size
    // Stricter tier for the public auth/account surface (login, register,
    // refresh, password-reset, token links). Those paths are auth-public, so
    // the general limiter's public_paths skip leaves them unthrottled — this
    // tier re-arms them with a tight per-IP cap.
    int protected_requests = 10;
    int protected_window_sec = 60;
    Scope scope = Scope::IpOrUser;
    bool trust_proxy = false;                   // read X-Forwarded-For
    int trusted_proxy_count = 1;                // # of trusted hops appended to XFF (index from the right)
    bool fail_open = true;                      // allow on Redis error
    std::unordered_set<std::string> whitelist;  // IPs or user subjects
    std::unordered_set<std::string> public_paths;
    std::unordered_set<std::string> protected_paths;  // auth surface, limited despite being public
};

struct Decision {
    bool allowed = true;
    int remaining = 0;
    int retry_after_sec = 0;
};

// Sliding-window counter via a Redis sorted set. Each request adds a
// (score=now_ms, member=now_ms_uuid) entry; the check is then a ZCOUNT
// over the trailing `window_sec * 1000` ms, plus a ZREMRANGEBYSCORE to
// drop stale entries. Done atomically inside a single EVAL so there is
// no TOCTOU between counting and inserting.
//
// Compared to fixed-window INCR, this prevents the classic 2x burst at
// the window boundary: 60 req in the last second of window N + 60 in the
// first of N+1 = 120 in 2 seconds. ZCOUNT integrates over a true 60-sec
// sliding window, so the cap is always honoured.
inline const char* kSlidingWindowLua = R"LUA(
-- KEYS[1] = zset key
-- ARGV[1] = now_ms
-- ARGV[2] = window_ms
-- ARGV[3] = limit
-- ARGV[4] = member to insert (unique)
-- Returns: { allowed(0|1), count_after, retry_after_ms }
local key = KEYS[1]
local now = tonumber(ARGV[1])
local window = tonumber(ARGV[2])
local limit = tonumber(ARGV[3])
local member = ARGV[4]
local cutoff = now - window
redis.call('ZREMRANGEBYSCORE', key, 0, cutoff)
local count = tonumber(redis.call('ZCARD', key) or 0)
if count >= limit then
  -- Need to wait until the oldest entry ages out.
  local oldest = redis.call('ZRANGE', key, 0, 0, 'WITHSCORES')
  local oldest_score = tonumber(oldest[2] or now)
  local retry_after_ms = (oldest_score + window) - now
  if retry_after_ms < 1 then retry_after_ms = 1 end
  return {0, count, retry_after_ms}
end
redis.call('ZADD', key, now, member)
redis.call('PEXPIRE', key, window + 1000)
return {1, count + 1, 0}
)LUA";

class Limiter {
public:
    explicit Limiter(Config cfg) : cfg_(std::move(cfg)) {}

    const Config& config() const { return cfg_; }

    // General tier: skips api.public_paths, buckets by ip_or_user.
    Decision check(const std::string& identity) const {
        return check_window(identity, cfg_.requests, cfg_.window_sec, "rl:sw:");
    }

    // Stricter tier for the public auth/account surface (login, register,
    // refresh, password-reset, token links). Separate Redis key namespace
    // ("rl:auth:") so it counts independently from the general limiter — a
    // burst of logins doesn't consume a user's normal API budget and vice
    // versa.
    Decision check_protected(const std::string& identity) const {
        return check_window(identity, cfg_.protected_requests, cfg_.protected_window_sec, "rl:auth:");
    }

private:
    Decision check_window(const std::string& identity, int requests, int window_sec, const char* key_prefix) const {
        if (!cfg_.enabled)
            return {true, requests, 0};
        // identity is prefixed ("ip:1.2.3.4" / "user:abc") but the configured
        // whitelist holds raw IPs / subjects — match against the bare value
        // (and the prefixed form, for forgiving config).
        if (cfg_.whitelist.count(identity) > 0) {
            return {true, requests, 0};
        }
        if (auto colon = identity.find(':');
            colon != std::string::npos && cfg_.whitelist.count(identity.substr(colon + 1)) > 0) {
            return {true, requests, 0};
        }
        if (!Cache::is_initialized()) {
            return fallback_("cache not initialized", requests);
        }
        try {
            const int64_t now_ms = Utils::Time::now_epoch_millis();
            const int64_t window_ms = static_cast<int64_t>(window_sec) * 1000;
            const std::string key = std::string(key_prefix) + identity;
            // member uniqueness = ms ts + atomic counter; two requests in
            // the same millisecond from the same identity get distinct members.
            static std::atomic<uint64_t> counter{0};
            const std::string member =
                std::to_string(now_ms) + ":" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

            auto& redis = Cache::get().get_client();
            // Redis 7+ auto-caches scripts; re-EVAL is cheap (script SHA cached client-side).
            std::vector<std::string> keys = {key};
            std::vector<std::string> args = {
                std::to_string(now_ms), std::to_string(window_ms), std::to_string(requests), member};
            auto result = redis.eval<std::vector<long long>>(
                kSlidingWindowLua, keys.begin(), keys.end(), args.begin(), args.end());
            if (result.size() < 3)
                return fallback_("sliding window eval: short reply", requests);

            const bool allowed = result[0] == 1;
            const long long count_after = result[1];
            const long long retry_ms = result[2];
            const int remaining = std::max(0, requests - static_cast<int>(count_after));
            const int retry_after_sec = allowed ? 0 : static_cast<int>(std::max<long long>(1, (retry_ms + 999) / 1000));
            return {allowed, remaining, retry_after_sec};
        } catch (const std::exception& e) {
            return fallback_(e.what(), requests);
        }
    }

    Decision fallback_(const std::string& reason, int requests) const {
        if (cfg_.fail_open) {
            spdlog::warn("rate limiter fail-open: {}", reason);
            return {true, requests, 0};
        }
        spdlog::warn("rate limiter fail-closed: {}", reason);
        return {false, 0, 1};
    }

    Config cfg_;
};

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------

inline std::unique_ptr<Limiter> global_limiter = nullptr;

inline Config load_config_from_global() {
    Config cfg;
    if (!::Config::is_initialized())
        return cfg;
    auto& c = ::Config::get();
    cfg.enabled = c.get<bool>("rate_limit.enabled", "RATE_LIMIT_ENABLED", false);
    cfg.requests = c.get<int>("rate_limit.requests", "RATE_LIMIT_REQUESTS", 60);
    cfg.window_sec = c.get<int>("rate_limit.window_sec", "RATE_LIMIT_WINDOW_SEC", 60);
    cfg.protected_requests = c.get<int>("rate_limit.protected_requests", "RATE_LIMIT_PROTECTED_REQUESTS", 10);
    cfg.protected_window_sec = c.get<int>("rate_limit.protected_window_sec", "RATE_LIMIT_PROTECTED_WINDOW_SEC", 60);
    cfg.trust_proxy = c.get<bool>("rate_limit.trust_proxy", "RATE_LIMIT_TRUST_PROXY", false);
    cfg.trusted_proxy_count = c.get<int>("rate_limit.trusted_proxy_count", "RATE_LIMIT_TRUSTED_PROXY_COUNT", 1);
    if (cfg.trusted_proxy_count < 1)
        cfg.trusted_proxy_count = 1;
    cfg.fail_open = c.get<bool>("rate_limit.fail_open", "RATE_LIMIT_FAIL_OPEN", true);
    cfg.scope = parse_scope(c.get<std::string>("rate_limit.scope", "RATE_LIMIT_SCOPE", "ip_or_user"));
    cfg.whitelist =
        Utils::Strings::split_csv_set(c.get<std::string>("rate_limit.whitelist", "RATE_LIMIT_WHITELIST", ""));
    // Single source of truth — see comment in Auth.hpp::load_config_from_global().
    cfg.public_paths = Utils::Strings::split_csv_set(
        c.get<std::string>("api.public_paths", "API_PUBLIC_PATHS", Utils::Strings::kDefaultPublicPathsCsv));
    cfg.protected_paths = Utils::Strings::split_csv_set(c.get<std::string>(
        "rate_limit.protected_paths", "RATE_LIMIT_PROTECTED_PATHS", Utils::Strings::kDefaultProtectedPathsCsv));
    if (cfg.requests <= 0)
        cfg.requests = 1;
    if (cfg.window_sec <= 0)
        cfg.window_sec = 1;
    if (cfg.protected_requests <= 0)
        cfg.protected_requests = 1;
    if (cfg.protected_window_sec <= 0)
        cfg.protected_window_sec = 1;
    return cfg;
}

inline void initialize() {
    if (global_limiter != nullptr) {
        // Warned no-op on repeated initialize — same convention as Auth and
        // Idempotency. Call shutdown() first to reconfigure.
        spdlog::warn("RateLimit::initialize called twice — keeping existing config");
        return;
    }
    auto cfg = load_config_from_global();
    global_limiter = std::make_unique<Limiter>(std::move(cfg));
    const auto& c = global_limiter->config();
    spdlog::info("Rate limiter initialized: enabled={} requests={} window_sec={}", c.enabled, c.requests, c.window_sec);
}

inline bool is_initialized() {
    return global_limiter != nullptr;
}

inline Limiter& get() {
    if (global_limiter == nullptr) {
        throw std::runtime_error("RateLimit not initialized");
    }
    return *global_limiter;
}

inline void shutdown() {
    global_limiter.reset();
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

inline std::string client_ip(const drogon::HttpRequestPtr& req, bool trust_proxy, int trusted_proxy_count = 1) {
    if (trust_proxy) {
        // Prefer X-Real-IP: our nginx sets it to $remote_addr (the real
        // upstream hop). The LEFTMOST X-Forwarded-For entry is fully client-
        // controlled — taking it lets an attacker rotate a fake IP per request
        // and dodge the limiter entirely. If only XFF is present, the real
        // client is the entry *trusted_proxy_count* hops from the right (each
        // trusted proxy appends one). With the default 1 that's the rightmost,
        // i.e. the address our single trusted proxy appended; behind 2 proxies
        // (LB → nginx) set it to 2 so all clients don't collapse into one
        // bucket (the inner proxy's constant address).
        const auto& xri = req->getHeader("X-Real-IP");
        if (!xri.empty())
            return xri;
        const auto& xff = req->getHeader("X-Forwarded-For");
        if (!xff.empty()) {
            // Split on commas, then pick the entry trusted_proxy_count from the end.
            std::vector<std::string> hops;
            size_t start = 0;
            while (start <= xff.size()) {
                size_t comma = xff.find(',', start);
                std::string tok = xff.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                size_t a = tok.find_first_not_of(" \t");
                size_t b = tok.find_last_not_of(" \t");
                if (a != std::string::npos)
                    hops.push_back(tok.substr(a, b - a + 1));
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            if (!hops.empty()) {
                int idx = static_cast<int>(hops.size()) - trusted_proxy_count;
                if (idx < 0)
                    idx = 0;  // fewer hops than trusted proxies → take the leftmost we have
                return hops[static_cast<size_t>(idx)];
            }
        }
    }
    return req->peerAddr().toIp();
}

// Convenience: resolve trust_proxy / trusted_proxy_count straight from app
// config, so any caller (audit, request logging, …) gets the SAME trusted
// client-IP logic as the limiter — even when rate limiting itself is disabled
// (the limiter may not be initialized, but the config keys still apply). With
// trust_proxy=false this returns peerAddr (the proxy hop) rather than a
// spoofable, client-supplied X-Real-IP header.
inline std::string client_ip(const drogon::HttpRequestPtr& req) {
    bool trust_proxy = false;
    int count = 1;
    if (::Config::is_initialized()) {
        auto& c = ::Config::get();
        trust_proxy = c.get<bool>("rate_limit.trust_proxy", "RATE_LIMIT_TRUST_PROXY", false);
        count = c.get<int>("rate_limit.trusted_proxy_count", "RATE_LIMIT_TRUSTED_PROXY_COUNT", 1);
        if (count < 1)
            count = 1;
    }
    return client_ip(req, trust_proxy, count);
}

inline std::string identity_for(const drogon::HttpRequestPtr& req, const Config& cfg) {
    const std::string ip = client_ip(req, cfg.trust_proxy, cfg.trusted_proxy_count);
    auto principal = Security::Auth::principal_of(req);
    const std::string user = (principal && !principal->subject.empty()) ? principal->subject : "";
    if (cfg.scope == Scope::Ip || user.empty()) {
        return "ip:" + ip;
    }
    return "user:" + user;
}

// Always-by-IP identity for the protected tier. Login/register carry no
// authenticated principal, and we don't want a (possibly attacker-supplied,
// expired) cookie's subject to shift the bucket — the brute-force unit is the
// source IP regardless of the configured scope.
inline std::string ip_identity(const drogon::HttpRequestPtr& req, const Config& cfg) {
    return "ip:" + client_ip(req, cfg.trust_proxy, cfg.trusted_proxy_count);
}

}  // namespace Security::RateLimit
