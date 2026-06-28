/**
 * @file Cache.hpp
 * @brief Cache module for Redis integration
 * @details Provides Redis operations with Sentinel support for high availability
 *          using redis-plus-plus library
 */

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

#include <nlohmann/json.hpp>

#include "utils/Strings.hpp"

namespace Cache {

using namespace sw::redis;
using namespace std::chrono_literals;

struct RedisAddress {
    std::string host = "127.0.0.1";
    int port = 6379;
};

/**
 * @brief Parse a Redis connection string ("tcp://host:port"; scheme and port
 *        optional). Shared by Cache::initialize and the worker's blocking
 *        client so the parsing (and its error handling) lives in one place.
 * @throws std::runtime_error on a malformed port.
 */
inline RedisAddress parse_redis_url(const std::string& url) {
    RedisAddress out;
    std::string addr = url;
    if (addr.starts_with("tcp://"))
        addr = addr.substr(6);
    auto colon = addr.find(':');
    if (colon == std::string::npos) {
        if (!addr.empty())
            out.host = addr;
        return out;
    }
    out.host = addr.substr(0, colon);
    const std::string port_str = addr.substr(colon + 1);
    try {
        out.port = std::stoi(port_str);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid Redis port in connection string: '" + port_str + "'");
    }
    return out;
}

/**
 * @brief Parse a "host:port,host:port" Sentinel node list. Entries without
 *        a valid port are skipped with a warning rather than aborting boot.
 */
inline std::vector<std::pair<std::string, int>> parse_sentinel_nodes_csv(const std::string& csv) {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& node : Utils::Strings::split_csv_vec(csv)) {
        auto colon = node.find(':');
        if (colon == std::string::npos) {
            spdlog::warn("Sentinel node '{}' has no port — skipping", node);
            continue;
        }
        try {
            out.emplace_back(node.substr(0, colon), std::stoi(node.substr(colon + 1)));
        } catch (const std::exception&) {
            spdlog::warn("Sentinel node '{}' has an invalid port — skipping", node);
        }
    }
    return out;
}

/**
 * @brief Build a standalone Redis client. Shared between Cache and the
 *        Jobs blocking-BRPOP client so connection params live in one place.
 */
inline std::unique_ptr<Redis> make_standalone_client(const std::string& host,
                                                     int port,
                                                     size_t pool_size,
                                                     const std::string& password,
                                                     std::chrono::milliseconds socket_timeout,
                                                     std::chrono::milliseconds pool_wait_timeout) {
    ConnectionOptions opts;
    opts.host = host;
    opts.port = port;
    opts.socket_timeout = socket_timeout;
    if (!password.empty())
        opts.password = password;
    ConnectionPoolOptions pool_opts;
    pool_opts.size = pool_size;
    pool_opts.wait_timeout = pool_wait_timeout;
    return std::make_unique<Redis>(opts, pool_opts);
}

/**
 * @brief Build a Sentinel-aware Redis client. Shared between Cache and Jobs.
 */
inline std::unique_ptr<Redis> make_sentinel_client(const std::string& master_name,
                                                   const std::vector<std::pair<std::string, int>>& sentinels,
                                                   size_t pool_size,
                                                   const std::string& password,
                                                   const std::string& sentinel_password,
                                                   std::chrono::milliseconds socket_timeout,
                                                   std::chrono::milliseconds pool_wait_timeout) {
    const std::string effective_sentinel_pw = sentinel_password.empty() ? password : sentinel_password;
    SentinelOptions sentinel_opts;
    for (const auto& [host, port] : sentinels) {
        sentinel_opts.nodes.push_back({host, port});
    }
    sentinel_opts.connect_timeout = 200ms;
    sentinel_opts.socket_timeout = 200ms;
    if (!effective_sentinel_pw.empty())
        sentinel_opts.password = effective_sentinel_pw;

    ConnectionOptions conn_opts;
    conn_opts.connect_timeout = 200ms;
    conn_opts.socket_timeout = socket_timeout;
    if (!password.empty())
        conn_opts.password = password;

    ConnectionPoolOptions pool_opts;
    pool_opts.size = pool_size;
    pool_opts.wait_timeout = pool_wait_timeout;

    auto sentinel = std::make_shared<Sentinel>(sentinel_opts);
    return std::make_unique<Redis>(sentinel, master_name, Role::MASTER, conn_opts, pool_opts);
}

/**
 * @brief Redis cache manager with Sentinel support
 *
 * Error contract: the wrapper methods (get/set/del/exists/expire/ttl/
 * sadd/smembers/zadd/publish/...) are FAIL-OPEN — they swallow
 * sw::redis::Error, log a warning and return a false/empty default, so a
 * Redis outage degrades features instead of failing requests. Callers that
 * need the failure signal must check the RETURN VALUE (see
 * AuthController::mint_session). Two deliberate exceptions: incr()/decr()
 * RETHROW — counters silently stuck at a default would corrupt rate
 * accounting. Direct get_client() calls bypass all of this: wrap them in
 * try/catch yourself.
 */
class CacheManager {
private:
    std::unique_ptr<Redis> redis_client;
    bool use_sentinel = false;
    bool initialized = false;

public:
    // Polymorphic so tests can substitute an in-memory fake (see
    // tests/InMemoryCache.hpp) for the singleton via
    // Cache::install_for_testing — the data ops below are virtual.
    virtual ~CacheManager() = default;

    /**
     * @brief Initialize Redis cache
     * @param connection_str Redis connection string (e.g., "tcp://127.0.0.1:6379")
     * @param pool_size Connection pool size
     */
    void initialize(const std::string& connection_str,
                    size_t pool_size = 10,
                    const std::string& password = "",
                    std::chrono::milliseconds socket_timeout = 500ms,
                    std::chrono::milliseconds pool_wait_timeout = 500ms) {
        if (initialized) {
            throw std::runtime_error("Cache already initialized");
        }
        try {
            const RedisAddress addr = parse_redis_url(connection_str);
            redis_client =
                make_standalone_client(addr.host, addr.port, pool_size, password, socket_timeout, pool_wait_timeout);
            redis_client->ping();
            initialized = true;
            use_sentinel = false;
            spdlog::info("Redis cache initialized (standalone: {}:{})", addr.host, addr.port);
        } catch (const Error& e) {
            spdlog::error("Failed to initialize Redis cache: {}", e.what());
            throw std::runtime_error("Redis initialization failed: " + std::string(e.what()));
        }
    }

    /**
     * @brief Initialize Redis with Sentinel for high availability
     * @param master_name Master service name
     * @param sentinels Vector of sentinel addresses (host, port)
     * @param pool_size Connection pool size
     */
    void initialize_with_sentinel(const std::string& master_name,
                                  const std::vector<std::pair<std::string, int>>& sentinels,
                                  size_t pool_size = 10,
                                  const std::string& password = "",
                                  const std::string& sentinel_password = "",
                                  std::chrono::milliseconds socket_timeout = 500ms,
                                  std::chrono::milliseconds pool_wait_timeout = 500ms) {
        if (initialized) {
            throw std::runtime_error("Cache already initialized");
        }
        try {
            redis_client = make_sentinel_client(
                master_name, sentinels, pool_size, password, sentinel_password, socket_timeout, pool_wait_timeout);
            redis_client->ping();
            initialized = true;
            use_sentinel = true;
            spdlog::info("Redis cache initialized with Sentinel (master: {})", master_name);
        } catch (const Error& e) {
            spdlog::error("Failed to initialize Redis with Sentinel: {}", e.what());
            throw std::runtime_error("Redis Sentinel initialization failed: " + std::string(e.what()));
        }
    }

    virtual bool set(const std::string& key, const std::string& value, long ttl = 0) {
        check_initialized();
        try {
            if (ttl > 0) {
                redis_client->setex(key, ttl, value);
            } else {
                redis_client->set(key, value);
            }
            return true;
        } catch (const Error& e) {
            spdlog::error("Failed to set key '{}': {}", key, e.what());
            return false;
        }
    }

    /**
     * @brief Atomic SET-if-not-exists with TTL — used as a lightweight
     *        distributed lock primitive. Returns true if the caller now
     *        holds the key, false if another writer got there first.
     *        On Redis error returns false (treat as "lock not acquired"
     *        to avoid double-processing during outages).
     */
    virtual bool set_nx(const std::string& key, const std::string& value, std::chrono::milliseconds ttl) {
        check_initialized();
        try {
            return redis_client->set(key, value, ttl, sw::redis::UpdateType::NOT_EXIST);
        } catch (const Error& e) {
            spdlog::error("Failed to SET NX key '{}': {}", key, e.what());
            return false;
        }
    }

    virtual std::optional<std::string> get(const std::string& key) {
        check_initialized();
        try {
            auto val = redis_client->get(key);
            if (val) {
                return *val;
            }
            return std::nullopt;
        } catch (const Error& e) {
            spdlog::error("Failed to get key '{}': {}", key, e.what());
            return std::nullopt;
        }
    }

    virtual long del(const std::string& key) {
        check_initialized();
        try {
            return redis_client->del(key);
        } catch (const Error& e) {
            spdlog::error("Failed to delete key '{}': {}", key, e.what());
            return 0;
        }
    }

    virtual long del(const std::vector<std::string>& keys) {
        check_initialized();
        try {
            return redis_client->del(keys.begin(), keys.end());
        } catch (const Error& e) {
            spdlog::error("Failed to delete multiple keys: {}", e.what());
            return 0;
        }
    }

    virtual bool exists(const std::string& key) {
        check_initialized();
        try {
            return redis_client->exists(key) > 0;
        } catch (const Error& e) {
            spdlog::error("Failed to check existence of key '{}': {}", key, e.what());
            return false;
        }
    }

    virtual bool expire(const std::string& key, long seconds) {
        check_initialized();
        try {
            return redis_client->expire(key, seconds);
        } catch (const Error& e) {
            spdlog::error("Failed to set expiration on key '{}': {}", key, e.what());
            return false;
        }
    }

    virtual long ttl(const std::string& key) {
        check_initialized();
        try {
            return redis_client->ttl(key);
        } catch (const Error& e) {
            spdlog::error("Failed to get TTL for key '{}': {}", key, e.what());
            return -2;
        }
    }

    virtual long long incr(const std::string& key, long long increment = 1) {
        check_initialized();
        try {
            if (increment == 1) {
                return redis_client->incr(key);
            } else {
                return redis_client->incrby(key, increment);
            }
        } catch (const Error& e) {
            spdlog::error("Failed to increment key '{}': {}", key, e.what());
            throw;
        }
    }

    virtual long long decr(const std::string& key, long long decrement = 1) {
        check_initialized();
        try {
            if (decrement == 1) {
                return redis_client->decr(key);
            } else {
                return redis_client->decrby(key, decrement);
            }
        } catch (const Error& e) {
            spdlog::error("Failed to decrement key '{}': {}", key, e.what());
            throw;
        }
    }

    long sadd(const std::string& key, const std::string& member) {
        check_initialized();
        try {
            return redis_client->sadd(key, member);
        } catch (const Error& e) {
            spdlog::error("Failed to add to set '{}': {}", key, e.what());
            return 0;
        }
    }

    std::vector<std::string> smembers(const std::string& key) {
        check_initialized();
        try {
            std::vector<std::string> members;
            redis_client->smembers(key, std::back_inserter(members));
            return members;
        } catch (const Error& e) {
            spdlog::error("Failed to get members of set '{}': {}", key, e.what());
            return {};
        }
    }

    long zadd(const std::string& key, const std::string& member, double score) {
        check_initialized();
        try {
            return redis_client->zadd(key, member, score);
        } catch (const Error& e) {
            spdlog::error("Failed to add to sorted set '{}': {}", key, e.what());
            return 0;
        }
    }

    long long publish(const std::string& channel, const std::string& message) {
        check_initialized();
        try {
            return redis_client->publish(channel, message);
        } catch (const Error& e) {
            spdlog::error("Failed to publish to channel '{}': {}", channel, e.what());
            return 0;
        }
    }

    bool health_check() {
        if (!initialized)
            return false;
        try {
            redis_client->ping();
            return true;
        } catch (const Error& e) {
            spdlog::error("Cache health check failed: {}", e.what());
            return false;
        }
    }

    void shutdown() {
        if (initialized) {
            spdlog::info("Shutting down cache manager");
            redis_client.reset();
            initialized = false;
            use_sentinel = false;
        }
    }

    virtual bool is_initialized() const { return initialized; }

    Redis& get_client() {
        check_initialized();
        return *redis_client;
    }

private:
    void check_initialized() const {
        if (!initialized) {
            throw std::runtime_error("Cache not initialized");
        }
    }
};

/**
 * @brief Global cache instance
 */
inline std::unique_ptr<CacheManager> global_cache = nullptr;

inline void initialize(const std::string& connection_str,
                       size_t pool_size = 10,
                       const std::string& password = "",
                       std::chrono::milliseconds socket_timeout = 500ms,
                       std::chrono::milliseconds pool_wait_timeout = 500ms) {
    if (global_cache != nullptr) {
        throw std::runtime_error("Cache already initialized");
    }
    global_cache = std::make_unique<CacheManager>();
    global_cache->initialize(connection_str, pool_size, password, socket_timeout, pool_wait_timeout);
}

inline void initialize_with_sentinel(const std::string& master_name,
                                     const std::vector<std::pair<std::string, int>>& sentinels,
                                     size_t pool_size = 10,
                                     const std::string& password = "",
                                     const std::string& sentinel_password = "",
                                     std::chrono::milliseconds socket_timeout = 500ms,
                                     std::chrono::milliseconds pool_wait_timeout = 500ms) {
    if (global_cache != nullptr) {
        throw std::runtime_error("Cache already initialized");
    }
    global_cache = std::make_unique<CacheManager>();
    global_cache->initialize_with_sentinel(
        master_name, sentinels, pool_size, password, sentinel_password, socket_timeout, pool_wait_timeout);
}

inline CacheManager& get() {
    if (global_cache == nullptr) {
        throw std::runtime_error("Cache not initialized");
    }
    return *global_cache;
}

inline bool is_initialized() {
    return global_cache != nullptr && global_cache->is_initialized();
}

inline void shutdown() {
    if (global_cache) {
        global_cache->shutdown();
        global_cache.reset();
    }
}

// ── Test seam ────────────────────────────────────────────────────────────────
// Swap the global cache for a fake (CacheManager subclass, e.g.
// tests/InMemoryCache.hpp) so cache-aside and fail-open paths are
// unit-testable without a live Redis. Call reset_for_testing() in TearDown.
inline void install_for_testing(std::unique_ptr<CacheManager> fake) {
    global_cache = std::move(fake);
}
inline void reset_for_testing() {
    global_cache.reset();
}

// ── Read-through (cache-aside) helper ─────────────────────────────────────────
// Return the cached value for `key`, or call `loader`, cache its result for
// `ttl_sec`, and return it. T must be nlohmann-serializable (to_json/from_json
// via ADL — every Domain DTO already is). Centralizes the get→miss→load→set
// dance so each call site (and each fork) doesn't hand-roll it differently.
//
// FAIL-OPEN by design: if the cache is uninitialized/down, or the cached blob is
// unparseable (e.g. the DTO's shape changed across a deploy), fall through to
// loader() and just skip caching — correctness never depends on Redis. Only
// cache positive lookups here; callers that must cache "absent" should wrap T in
// std::optional and let from_json handle null.
template <typename T, typename Loader>
T cached(const std::string& key, long ttl_sec, Loader&& loader) {
    if (is_initialized()) {
        try {
            if (auto hit = get().get(key))
                return nlohmann::json::parse(*hit).template get<T>();
        } catch (const std::exception& e) {
            spdlog::debug("cache: ignoring unusable entry for '{}' ({})", key, e.what());
        }
    }
    T value = std::forward<Loader>(loader)();
    if (is_initialized()) {
        try {
            get().set(key, nlohmann::json(value).dump(), ttl_sec);
        } catch (const std::exception& e) {
            spdlog::debug("cache: failed to store '{}' ({})", key, e.what());
        }
    }
    return value;
}

}  // namespace Cache
