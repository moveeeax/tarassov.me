#pragma once

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "core/Core.hpp"
#include "database/Database.hpp"
#include "database/Migrations.hpp"
#include "jobs/Jobs.hpp"
#include "messaging/Messaging.hpp"
#include "observability/Observability.hpp"
#include "security/Auth.hpp"
#include "tasks/Tasks.hpp"
#include "utils/Config.hpp"

namespace TestHelpers {

// Test key prefix for Redis isolation
inline const std::string TEST_KEY_PREFIX = "test:";

inline std::string test_key(const std::string& name) {
    return TEST_KEY_PREFIX + name;
}

// Read infrastructure hostnames from env vars (for Docker network)
inline std::string pg_host() {
    const char* h = std::getenv("TEST_PG_HOST");
    return h ? h : "localhost";
}

inline std::string redis_host() {
    const char* h = std::getenv("TEST_REDIS_HOST");
    return h ? h : "127.0.0.1";
}

inline int pg_port() {
    const char* p = std::getenv("TEST_PG_PORT");
    return p ? std::atoi(p) : 5432;
}

inline int redis_port() {
    const char* p = std::getenv("TEST_REDIS_PORT");
    return p ? std::atoi(p) : 6379;
}

inline std::string pg_conn_string() {
    return "postgresql://postgres:postgres@" + pg_host() + ":" + std::to_string(pg_port()) + "/appdb";
}

inline std::string redis_url() {
    return "tcp://" + redis_host() + ":" + std::to_string(redis_port());
}

/**
 * @brief Process-wide metrics-port allocator. Every fixture that boots
 *        Observability must use a fresh port: rebinding the same one
 *        immediately after the previous test's shutdown races the socket's
 *        TIME_WAIT. One counter (instead of per-fixture statics with magic
 *        disjoint bases) can't collide as suites grow.
 */
inline int next_metrics_port() {
    static std::atomic<int> port{19100};
    return port.fetch_add(1);
}

// Minimal config JSON for tests (uses env-aware hosts)
inline std::string minimal_config() {
    return R"({
    "logging": {
        "name": "test",
        "file": "logs/test.log",
        "level": "warn"
    },
    "observability": {
        "metrics_address": "0.0.0.0:)" +
           std::to_string(next_metrics_port()) + R"(",
        "service_name": "test_service"
    },
    "async": {
        "thread_count": 2
    },
    "database": {
        "primary": ")" +
           pg_conn_string() + R"(",
        "pool_size": 2
    },
    "cache": {
        "url": ")" +
           redis_url() + R"(",
        "pool_size": 2,
        "use_sentinel": false
    },
    "messaging": {
        "enabled": false
    }
})";
}

/**
 * @brief RAII env-var override: sets on construction, restores the previous
 *        value (or unsets) on destruction — exception-safe, unlike raw
 *        setenv/unsetenv pairs scattered through test bodies.
 */
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
        save_();
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
    }

    /// Unset variant: removes the variable for the scope's lifetime.
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        save_();
        ::unsetenv(name_.c_str());
    }
    ~ScopedEnv() {
        if (had_previous_)
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void save_() {
        const char* prev = std::getenv(name_.c_str());
        if (prev) {
            had_previous_ = true;
            previous_ = prev;
        }
    }

    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

// Counter for unique logger names after reset
inline int reset_counter_ = 0;

/**
 * @brief Re-install a default spdlog logger. Needed after anything calls
 *        spdlog::shutdown() (Observability teardown) — subsequent
 *        spdlog::info() would otherwise touch a dead default logger.
 */
inline void restore_default_spdlog() {
    try {
        auto fallback = spdlog::stdout_color_mt("fallback_" + std::to_string(reset_counter_++));
        fallback->set_level(spdlog::level::warn);
        spdlog::set_default_logger(fallback);
    } catch (...) {}
}

// Force-teardown all singletons in reverse init order.
// Each module's shutdown() is a no-op when the global is already null, so we
// call them unconditionally — the is_initialized() guards miss the case where
// a previous test left a non-null global with initialized=false (e.g. after
// a partial Core::initialize() that threw).
inline void reset_all_globals() {
    try {
        Core::shutdown();
    } catch (...) {}
    // Clear the graceful-shutdown latch — otherwise a test that called
    // Core::begin_shutdown() leaves every later /ready returning 503.
    Core::shutting_down_flag.store(false);
    try {
        Jobs::shutdown();
    } catch (...) {}
    try {
        Tasks::shutdown();
    } catch (...) {}
    try {
        Messaging::shutdown();
    } catch (...) {}
    try {
        Cache::shutdown();
    } catch (...) {}
    try {
        Migrations::shutdown();
    } catch (...) {}
    try {
        Database::shutdown();
    } catch (...) {}
    // Unbind Retry's metric sink BEFORE Observability::shutdown so any
    // subsequent retry event doesn't deref the freed Prometheus family.
    try {
        Retry::reset_metrics();
    } catch (...) {}
    try {
        Database::reset_query_metrics();
    } catch (...) {}
    try {
        Observability::shutdown();
    } catch (...) {}
    try {
        Config::shutdown();
    } catch (...) {}

    // Restore a default spdlog logger so subsequent code using spdlog::info() etc.
    // doesn't segfault after Observability::shutdown() called spdlog::shutdown()
    restore_default_spdlog();
}

// Infrastructure connectivity checks
inline bool is_postgres_available() {
    try {
        pqxx::connection conn(pg_conn_string());
        pqxx::nontransaction ntxn(conn);
        ntxn.exec("SELECT 1");
        return true;
    } catch (...) {
        return false;
    }
}

inline bool is_redis_available() {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = redis_host();
        opts.port = redis_port();
        opts.socket_timeout = std::chrono::milliseconds(500);
        sw::redis::Redis redis(opts);
        redis.ping();
        return true;
    } catch (...) {
        return false;
    }
}

// Temp config file helpers. Pass a distinct filename when suites may run
// interleaved in the same process/working dir.
inline std::string create_temp_config(const std::string& content, const std::string& path = "test_temp_config.json") {
    std::ofstream file(path);
    file << content;
    file.close();
    return path;
}

inline std::string create_temp_config() {
    return create_temp_config(minimal_config());
}

inline void remove_temp_config(const std::string& path = "test_temp_config.json") {
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }
}

// ---------------------------------------------------------------------------
// Shared cleanup helpers — one definition instead of per-suite copies that
// drift (an early copy forgot the jobs:index zsets and leaked into the
// global index).
// ---------------------------------------------------------------------------

/// Wipe the users table between tests. Requires Database to be initialized.
/// CASCADE so tables with an FK to users (api_keys, and any owner-scoped
/// resource a fork adds) are cleared too — plain TRUNCATE errors on a referenced
/// table.
inline void truncate_users() {
    Database::get().execute_write([](auto& txn) {
        txn.exec("TRUNCATE TABLE users CASCADE");
        return 0;
    });
}

/**
 * @brief Drain job state for the given queue types: queued ids, their job
 *        blobs, the per-type queue/DLQ/index keys, and the ids' entries in
 *        the global jobs:index. Requires Cache to be initialized.
 */
inline void drain_jobs(const std::vector<std::string>& types) {
    auto& redis = Cache::get().get_client();
    for (const auto& type : types) {
        std::vector<std::string> ids;
        try {
            redis.lrange(Jobs::queue_key(type), 0, -1, std::back_inserter(ids));
            redis.lrange(Jobs::dlq_key(type), 0, -1, std::back_inserter(ids));
            redis.zrange(Jobs::index_key_for(type), 0, -1, std::back_inserter(ids));
        } catch (...) {}
        for (const auto& id : ids) {
            try {
                redis.del(Jobs::job_key(id));
                redis.zrem(Jobs::index_key(), id);
                redis.srem(Jobs::kDlqAllKey, id);
            } catch (...) {}
        }
        try {
            redis.del(Jobs::queue_key(type));
            redis.del(Jobs::dlq_key(type));
            redis.del(Jobs::index_key_for(type));
        } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// Request builders — drive controllers via these instead of hand-rolling
// HttpRequest construction in every test (the CONVENTIONS gotchas).
// ---------------------------------------------------------------------------

/// Bare request with the given method.
inline drogon::HttpRequestPtr make_request(drogon::HttpMethod method = drogon::Get) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    return req;
}

/// JSON-body request (defaults to POST). Sets Content-Type.
inline drogon::HttpRequestPtr make_request(drogon::HttpMethod method, const nlohmann::json& body) {
    auto req = make_request(method);
    req->setBody(body.dump());
    req->addHeader("Content-Type", "application/json");
    return req;
}

inline drogon::HttpRequestPtr post_json(const nlohmann::json& body) {
    return make_request(drogon::Post, body);
}

/// Stamp a principal onto a request, mirroring what the auth middleware
/// does after verifying an access token.
inline drogon::HttpRequestPtr with_principal(drogon::HttpRequestPtr req, const Security::Auth::AuthPrincipal& p) {
    req->attributes()->insert(Security::Auth::kPrincipalAttr, p);
    return req;
}

inline drogon::HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, drogon::HttpMethod method = drogon::Get) {
    return with_principal(make_request(method), p);
}

inline drogon::HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                          const nlohmann::json& body,
                                          drogon::HttpMethod method = drogon::Post) {
    return with_principal(make_request(method, body), p);
}

// ---------------------------------------------------------------------------
// Shared fixture base: boots Core from minimal_config() + per-suite overrides,
// skips when Postgres/Redis sidecars are unreachable, and tears everything
// down. Derive and override config_overrides() to flip feature flags.
// ---------------------------------------------------------------------------

class CoreBackedTest : public ::testing::Test {
protected:
    std::string config_path_;

    /// Override to patch the minimal config (e.g. cfg["jobs"]["enabled"] = true).
    virtual void config_overrides(nlohmann::json& /*cfg*/) {}

    /// Override to use a distinct file name when suites run interleaved.
    virtual std::string config_file_name() const { return "core_backed_test_config.json"; }

    /// Override to false for suites that only need Redis (e.g. pure job-queue
    /// tests) — they then run in Redis-only environments too.
    virtual bool requires_postgres() const { return true; }

    /// Hook running right after Core::initialize — e.g. to attach a dedicated
    /// blocking Redis client for BRPOP-based tests.
    virtual void post_init() {}

    void SetUp() override {
        const bool pg_ok = !requires_postgres() || is_postgres_available();
        const bool redis_ok = is_redis_available();
        if (!pg_ok || !redis_ok) {
            // Locally, skipping when infra is absent is convenient. In CI it is a
            // trap: a mis-wired integration job (no Postgres/Redis service) would
            // SKIP every suite and report all-green, silently disabling the whole
            // integration safety net. Set CI_REQUIRE_INFRA=1 in CI so a missing
            // dependency FAILS loudly instead.
            const char* require_infra = std::getenv("CI_REQUIRE_INFRA");
            const bool must_have_infra = require_infra != nullptr && std::string(require_infra) != "" &&
                                         std::string(require_infra) != "0" && std::string(require_infra) != "false";
            if (must_have_infra) {
                FAIL() << "CI_REQUIRE_INFRA is set but test infra is unavailable (postgres_ok=" << pg_ok
                       << ", redis_ok=" << redis_ok
                       << "). This suite would have SKIPPED — failing instead so a mis-configured "
                          "integration job can't report all-green.";
            }
            GTEST_SKIP() << "Postgres or Redis not available";
        }
        // Defensive: if the previous suite died before its TearDown,
        // Core::initialize would throw "already initialized" and cascade the
        // whole suite. reset_all_globals() is idempotent by design.
        reset_all_globals();
        nlohmann::json cfg = nlohmann::json::parse(minimal_config());
        config_overrides(cfg);
        config_path_ = config_file_name();
        std::ofstream(config_path_) << cfg.dump(2);
        Core::initialize(config_path_);
        post_init();
    }

    void TearDown() override {
        reset_all_globals();
        if (!config_path_.empty() && std::filesystem::exists(config_path_))
            std::filesystem::remove(config_path_);
    }
};

}  // namespace TestHelpers
