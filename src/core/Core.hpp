/**
 * @file Core.hpp
 * @brief Core application module
 * @details Orchestrates initialization and shutdown of all subsystems
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "database/Migrations.hpp"
#include "email/Mailer.hpp"
#include "jobs/Jobs.hpp"
#include "messaging/Messaging.hpp"
#include "observability/Observability.hpp"
#include "security/Auth.hpp"
#include "security/Idempotency.hpp"
#include "security/RateLimit.hpp"
#include "storage/Storage.hpp"
#include "tasks/Tasks.hpp"
#include "utils/Config.hpp"
#include "utils/Pg.hpp"
#include "utils/Strings.hpp"
#include "version.hpp"

namespace Core {

/**
 * @brief Surface insecure database credentials at startup.
 * @details Parses a postgresql:// URL and extracts the password component.
 *          If the password is empty or matches a known-weak default
 *          ("postgres", "password", etc.), logs a loud warn. We can't
 *          hard-fail by default because docker-compose ships
 *          postgres:postgres for the local dev stack — flip
 *          DATABASE_REQUIRE_SECURE_PASSWORD=true in any non-dev
 *          environment and the check becomes a hard error.
 *          libpq key=value-form connection strings and URLs without
 *          userinfo (peer auth, certificate auth) are skipped.
 */
inline void check_password_value(const std::string& password) {
    static const std::unordered_set<std::string> kWeak = {
        "", "postgres", "password", "changeme", "admin", "root", "123456"};
    if (kWeak.count(password) == 0)
        return;

    const char* enforce = std::getenv("DATABASE_REQUIRE_SECURE_PASSWORD");
    const bool require_secure = enforce != nullptr && (std::string(enforce) == "true" || std::string(enforce) == "1" ||
                                                       std::string(enforce) == "yes");
    const std::string msg =
        "Database password is empty or matches a known-weak default — set DATABASE_PASSWORD "
        "to a strong secret. Set DATABASE_REQUIRE_SECURE_PASSWORD=true to make this fatal.";
    if (require_secure) {
        spdlog::critical(msg);
        throw std::runtime_error("insecure database password rejected by DATABASE_REQUIRE_SECURE_PASSWORD");
    }
    spdlog::warn(msg);
}

inline void check_password_safety(const std::string& url) {
    if (!(url.starts_with("postgresql://") || url.starts_with("postgres://")))
        return;
    auto scheme_end = url.find("://");
    auto authority_end = url.find('@', scheme_end);
    if (authority_end == std::string::npos)
        return;
    auto userinfo = url.substr(scheme_end + 3, authority_end - scheme_end - 3);
    auto colon = userinfo.find(':');
    std::string password = (colon == std::string::npos) ? "" : userinfo.substr(colon + 1);
    check_password_value(password);
}

/**
 * @brief Initialization mode
 */
enum class InitMode {
    Full,        // API server: all subsystems
    Worker,      // Worker process: skip Tasks, skip Messaging
    MigrateOnly  // Run migrations only: Config + Observability + Database + Migrations
};

/**
 * @brief Application lifecycle manager
 */
class Application {
private:
    bool initialized = false;
    std::string config_file;
    InitMode init_mode = InitMode::Full;

public:
    void initialize(const std::string& config_path, InitMode mode = InitMode::Full) {
        if (initialized) {
            throw std::runtime_error("Application already initialized");
        }
        config_file = config_path;
        init_mode = mode;

        try {
            Config::initialize(config_file);
            auto& cfg = Config::get();

            init_observability_(cfg);
            spdlog::info("=== Application Initialization Started ===");

            validate_config_(cfg);

            init_database_(cfg);
            init_migrations_(cfg, mode);
            if (mode == InitMode::MigrateOnly) {
                initialized = true;
                spdlog::info("=== Migrations completed (MigrateOnly mode) ===");
                return;
            }

            init_cache_(cfg);
            Storage::initialize(cfg);
            if (mode != InitMode::Worker) {
                init_messaging_(cfg);
                Tasks::initialize();
                register_token_reaper_();
                register_db_pool_metric_(cfg);
                register_replication_lag_metric_(cfg);
            }
            init_security_();
            init_jobs_(cfg);
            // Mailer comes up after Jobs because the typical flow is
            // Jobs::submit("email", payload) → worker → Mailer::send.
            // It's also useful in synchronous paths during dev.
            Email::initialize();
            register_default_health_checks_();

            initialized = true;
            spdlog::info("=== Application Initialization Complete ===");
        } catch (const std::exception& e) {
            spdlog::error("Application initialization failed: {}", e.what());
            shutdown();
            throw;
        }
    }

    // Exposed so the prod-safety gate can be unit-tested without a full boot.
    static void validate_config(Config::AppConfig& cfg) { validate_config_(cfg); }

private:
    // ---------------------------------------------------------------------
    // Per-subsystem initializers — each reads from config and brings up
    // exactly one module. Kept in initialize()'s lexical order for clarity.
    // ---------------------------------------------------------------------

    // Boot-time config sanity. A 12-factor app reads everything from env, so a
    // single typo can silently flip a security control. Fail LOUD (throw →
    // caught in initialize(), logged, process exits non-zero) on combinations
    // that are almost always mistakes in production, rather than starting up
    // quietly insecure. The auth.mode=jwt secret check lives in init_security_.
    static void validate_config_(Config::AppConfig& cfg) {
        const std::string env = cfg.get<std::string>("app.env", "APP_ENV", "development");
        const bool is_prod = (env == "production" || env == "prod");
        const std::string auth_mode = cfg.get<std::string>("auth.mode", "AUTH_MODE", "none");

        if (is_prod && auth_mode == "none") {
            throw std::runtime_error(
                "Config validation: auth.mode=none with APP_ENV=" + env +
                " — refusing to start a production service with every endpoint public. "
                "Set AUTH_MODE=jwt (or bearer), or set APP_ENV=development if this is genuinely intended.");
        }
        if (is_prod && auth_mode == "jwt") {
            const bool secure = cfg.get<bool>("auth.cookies.secure", "AUTH_COOKIE_SECURE", true);
            if (!secure)
                spdlog::warn(
                    "Config validation: auth.cookies.secure=false in production — __Host- cookies are "
                    "dropped and session cookies would travel over plaintext. Set AUTH_COOKIE_SECURE=true.");
        }
        // Production-safety checks the BINARY enforces regardless of which config
        // profile / env produced the values — so the Helm deploy path can't quietly
        // bypass them the way it bypasses prod-check.sh / env-check.sh (they only
        // run against config.production.json, never the gitignored values-prod.yaml).
        if (is_prod) {
            if (!cfg.get<bool>("rate_limit.enabled", "RATE_LIMIT_ENABLED", false))
                spdlog::warn(
                    "Config validation: rate_limit.enabled=false in production — /api/auth/login is "
                    "unthrottled (brute-force exposure). Set RATE_LIMIT_ENABLED=true.");
            else if (cfg.get<bool>("rate_limit.fail_open", "RATE_LIMIT_FAIL_OPEN", true))
                spdlog::warn(
                    "Config validation: rate_limit.fail_open=true in production — a Redis outage "
                    "silently disables the limiter. Set RATE_LIMIT_FAIL_OPEN=false.");
            if (cfg.get<bool>("docs.enabled", "DOCS_ENABLED", false))
                spdlog::warn(
                    "Config validation: docs.enabled=true in production — the API docs UI is publicly "
                    "exposed. Set DOCS_ENABLED=false.");
            // CSRF defense-in-depth when cookie auth is on (mutations otherwise lean
            // on SameSite=Lax alone).
            if (auth_mode == "jwt" && cfg.get<bool>("auth.cookies.enabled", "AUTH_COOKIES_ENABLED", false) &&
                !cfg.get<bool>("security.csrf.enabled", "SECURITY_CSRF_ENABLED", false))
                spdlog::warn(
                    "Config validation: cookie auth enabled but security.csrf.enabled=false in "
                    "production — mutations rely on SameSite=Lax only. Set SECURITY_CSRF_ENABLED=true.");
        }
        spdlog::info("Config validated (env={}, auth_mode={})", env, auth_mode);
    }

    static void init_observability_(Config::AppConfig& cfg) {
        auto log_name = cfg.get<std::string>("logging.name", "LOG_NAME", "cpp_api");
        auto log_file = cfg.get<std::string>("logging.file", "LOG_FILE", "logs/app.log");
        auto log_level = cfg.get<std::string>("logging.level", "LOG_LEVEL", "info");
        auto log_format = cfg.get<std::string>("logging.format", "LOG_FORMAT", "text");
        auto metrics_addr = cfg.get<std::string>("observability.metrics_address", "METRICS_ADDRESS", "0.0.0.0:9090");
        auto service_name = cfg.get<std::string>("observability.service_name", "SERVICE_NAME", "cpp_api_service");
        auto otlp_endpoint = cfg.get<std::string>("observability.otlp_endpoint", "OTLP_ENDPOINT", "");
        Observability::initialize(log_name, log_file, metrics_addr, service_name, otlp_endpoint, log_format);
        Observability::get().logger().set_level(log_level);

        // Bind the retries_total counter family. Retry::run stays free of
        // Prometheus headers — it calls into a plain function pointer we
        // hand it here. The lambda captures the family by reference (the
        // Observability singleton outlives every retry call).
        static prometheus::Family<prometheus::Counter>* retries_family = nullptr;
        retries_family = &Observability::get().metrics().create_counter(
            "retries_total", "Transient-error retries, labeled by component and outcome (retried|exhausted)");
        Retry::bind_metrics([](const char* component, const char* outcome) {
            if (retries_family == nullptr)
                return;
            try {
                retries_family->Add({{"component", component}, {"outcome", outcome}}).Increment();
            } catch (...) {}
        });
    }

    // DATABASE_REPLICA_URLS (env) is preferred for container deployments;
    // config JSON is a fallback for local dev where setting env is annoying.
    static std::vector<std::string> read_replicas_(Config::AppConfig& cfg) {
        std::vector<std::string> replicas;
        const char* replica_env = std::getenv("DATABASE_REPLICA_URLS");
        if (replica_env && std::strlen(replica_env) > 0) {
            for (auto& s : Utils::Strings::split_csv_vec(replica_env)) {
                replicas.push_back(std::move(s));
            }
            return replicas;
        }
        // Parts form: a CSV of replica HOSTS, assembled into DSNs with the same
        // port/user/dbname/password as the primary — so the password isn't baked
        // into a URL env var (see init_database_). Mirrors DATABASE_REPLICA_URLS.
        const char* replica_hosts = std::getenv("DATABASE_REPLICA_HOSTS");
        if (replica_hosts && std::strlen(replica_hosts) > 0) {
            const int port = cfg.get<int>("database.port", "DATABASE_PORT", 5432);
            const std::string user = cfg.get<std::string>("database.user", "DATABASE_USER", "app");
            const std::string name = cfg.get<std::string>("database.name", "DATABASE_NAME", "app");
            const std::string password = cfg.get<std::string>("database.password", "DATABASE_PASSWORD", "");
            for (auto& h : Utils::Strings::split_csv_vec(replica_hosts))
                replicas.push_back(Utils::Pg::make_conninfo(h, port, user, name, password));
            return replicas;
        }
        try {
            auto replicas_json = cfg.get_json().at("database").at("replicas");
            for (const auto& r : replicas_json)
                replicas.push_back(r.get<std::string>());
        } catch (...) {}
        return replicas;
    }

    static void init_database_(Config::AppConfig& cfg) {
        // Prefer a full DATABASE_PRIMARY_URL when given (backward compatible).
        // Otherwise assemble a libpq DSN from discrete parts so the password
        // lives ONLY in DATABASE_PASSWORD and is never materialized into a URL
        // env var (which would leak it via `kubectl exec -- env` / crash dumps).
        auto primary = cfg.get<std::string>("database.primary", "DATABASE_PRIMARY_URL", "");
        if (primary.empty()) {
            const std::string host = cfg.get<std::string>("database.host", "DATABASE_HOST", "localhost");
            const int port = cfg.get<int>("database.port", "DATABASE_PORT", 5432);
            const std::string user = cfg.get<std::string>("database.user", "DATABASE_USER", "app");
            const std::string name = cfg.get<std::string>("database.name", "DATABASE_NAME", "app");
            const std::string password = cfg.get<std::string>("database.password", "DATABASE_PASSWORD", "");
            check_password_value(password);
            primary = Utils::Pg::make_conninfo(host, port, user, name, password);
        } else {
            check_password_safety(primary);
        }
        int pool_size = cfg.get<int>("database.pool_size", "DB_POOL_SIZE", 10);
        int acquire_ms = cfg.get<int>("database.acquire_timeout_ms", "DB_ACQUIRE_TIMEOUT_MS", 5000);
        // 0 disables the per-connection PostgreSQL statement_timeout. Default
        // 30s is a sensible upper bound for transactional API queries — long
        // analytics/migration queries should run on a separate connection
        // string with timeout cleared.
        int stmt_timeout_ms = cfg.get<int>("database.statement_timeout_ms", "DB_STATEMENT_TIMEOUT_MS", 30000);

        Database::initialize(primary,
                             read_replicas_(cfg),
                             pool_size,
                             std::chrono::milliseconds(acquire_ms),
                             std::chrono::milliseconds(stmt_timeout_ms));

        // db_queries_total{op, pool}: makes replica routing visible on the
        // cpp-api Grafana dashboard. Same lifetime contract as retries_total.
        static prometheus::Family<prometheus::Counter>* db_queries_family = nullptr;
        db_queries_family = &Observability::get().metrics().create_counter(
            "db_queries_total",
            "Database operations by op (read|write|transaction) and serving pool (primary|replica)");
        Database::bind_query_metrics([](const char* op, const char* pool) {
            if (db_queries_family == nullptr)
                return;
            try {
                db_queries_family->Add({{"op", op}, {"pool", pool}}).Increment();
            } catch (...) {}
        });

        // Retry::run sleeps SYNCHRONOUSLY on the calling thread. On the API that
        // thread is a Drogon IO event loop, so a transient DB blip (failover,
        // lock spike) parks the loop for up to (max_attempts-1)*max_delay and
        // stalls UNRELATED requests on the same loop. Keep the request-path
        // defaults tight (2 attempts, 20→200ms ≈ 0.2s worst case) so a hiccup is
        // a brief latency bump, not a correlated cliff. The worker runs DB work
        // off the IO loops — raise DB_RETRY_* there if you want more retries.
        Retry::Policy p;
        p.max_attempts = cfg.get<int>("database.retry.max_attempts", "DB_RETRY_MAX_ATTEMPTS", 2);
        p.base_delay_ms = cfg.get<int>("database.retry.base_delay_ms", "DB_RETRY_BASE_DELAY_MS", 20);
        p.max_delay_ms = cfg.get<int>("database.retry.max_delay_ms", "DB_RETRY_MAX_DELAY_MS", 200);
        p.jitter = cfg.get<bool>("database.retry.jitter", "DB_RETRY_JITTER", true);
        Database::get().set_retry_policy(p);
    }

    static void init_migrations_(Config::AppConfig& cfg, InitMode mode) {
        // MigrateOnly always runs — DB_MIGRATIONS_ENABLED is a runtime hint for
        // the main app (where migrations are usually handled by a dedicated
        // init container).
        bool enabled = (mode == InitMode::MigrateOnly) ||
                       cfg.get<bool>("database.migrations_enabled", "DB_MIGRATIONS_ENABLED", true);
        if (!enabled)
            return;
        auto dir = cfg.get<std::string>("database.migrations_dir", "DB_MIGRATIONS_DIR", "migrations");
        Migrations::initialize(dir);
    }

    static std::vector<std::pair<std::string, int>> read_sentinels_(Config::AppConfig& cfg) {
        const char* env = std::getenv("REDIS_SENTINEL_NODES");
        if (env && std::strlen(env) > 0) {
            return Cache::parse_sentinel_nodes_csv(env);
        }
        std::vector<std::pair<std::string, int>> out;
        try {
            auto nodes = cfg.get_json().at("cache").at("sentinel").at("nodes");
            for (const auto& node : nodes) {
                out.emplace_back(node.at("host").get<std::string>(), node.at("port").get<int>());
            }
        } catch (...) {
            out.emplace_back("localhost", 26379);
        }
        return out;
    }

    static void init_cache_(Config::AppConfig& cfg) {
        bool use_sentinel = cfg.get<bool>("cache.use_sentinel", "REDIS_USE_SENTINEL", false);
        int pool_size = cfg.get<int>("cache.pool_size", "CACHE_POOL_SIZE", 10);
        auto password = cfg.get<std::string>("cache.password", "REDIS_PASSWORD", "");
        auto sentinel_password = cfg.get<std::string>("cache.sentinel.password", "REDIS_SENTINEL_PASSWORD", password);
        // Default 500ms — tight enough to fail a dead master quickly, but
        // generous enough for EVAL / SMEMBERS / large values under load.
        // Tune down in hot paths and up for analytics / big keyspaces.
        int socket_timeout_ms = cfg.get<int>("cache.socket_timeout_ms", "REDIS_SOCKET_TIMEOUT_MS", 500);
        int pool_wait_ms = cfg.get<int>("cache.pool_wait_timeout_ms", "REDIS_POOL_WAIT_TIMEOUT_MS", 500);
        auto sock_to = std::chrono::milliseconds(std::max(1, socket_timeout_ms));
        auto pool_to = std::chrono::milliseconds(std::max(1, pool_wait_ms));

        if (use_sentinel) {
            auto master = cfg.get<std::string>("cache.sentinel.master_name", "REDIS_MASTER_NAME", "mymaster");
            Cache::initialize_with_sentinel(
                master, read_sentinels_(cfg), pool_size, password, sentinel_password, sock_to, pool_to);
        } else {
            auto url = cfg.get<std::string>("cache.url", "REDIS_URL", "tcp://127.0.0.1:6379");
            Cache::initialize(url, pool_size, password, sock_to, pool_to);
        }
    }

    static std::vector<std::string> read_kafka_topics_(Config::AppConfig& cfg) {
        std::vector<std::string> topics;
        try {
            auto node = cfg.get_json().at("messaging").at("kafka").at("consumer").at("topics");
            for (const auto& t : node)
                topics.push_back(t.get<std::string>());
        } catch (...) {
            topics.push_back("default_topic");
        }
        return topics;
    }

    static void init_messaging_(Config::AppConfig& cfg) {
        if (!cfg.get<bool>("messaging.enabled", "MESSAGING_ENABLED", false))
            return;

        Messaging::initialize();
        auto brokers = cfg.get<std::string>("messaging.kafka.brokers", "KAFKA_BROKERS", "localhost:9092");

        if (cfg.get<bool>("messaging.kafka.producer.enabled", "KAFKA_PRODUCER_ENABLED", false)) {
            auto producer_id =
                cfg.get<std::string>("messaging.kafka.producer.client_id", "KAFKA_PRODUCER_ID", "cpp_producer");
            Messaging::get().initialize_producer(brokers, producer_id);
        }
        if (cfg.get<bool>("messaging.kafka.consumer.enabled", "KAFKA_CONSUMER_ENABLED", false)) {
            auto group_id =
                cfg.get<std::string>("messaging.kafka.consumer.group_id", "KAFKA_GROUP_ID", "cpp_consumer_group");
            Messaging::get().initialize_consumer(brokers, group_id, read_kafka_topics_(cfg));
        }
    }

    // Throws if auth.mode=jwt and no secret is set — refuse to silently start
    // a service that would accept unauthenticated traffic.
    static void init_security_() {
        Security::Auth::initialize();
        Security::RateLimit::initialize();
        Security::Idempotency::initialize();
    }

    // Registers jobs_dlq_depth as a Prometheus gauge, labeled by job type
    // so operators can spot which queue specifically is clogged. The
    // special label value `_total` carries the aggregate across every
    // type for single-stat widgets. Refreshed every N seconds from Redis.
    static void register_dlq_metric_(Config::AppConfig& cfg) {
        if (!Observability::is_initialized() || !Tasks::is_initialized())
            return;
        auto& family =
            Observability::get().metrics().create_gauge("jobs_dlq_depth",
                                                        "Current depth of the jobs dead-letter queue by type "
                                                        "(special label type=\"_total\" for the aggregate)");
        int refresh_sec = cfg.get<int>("jobs.dlq_metric_refresh_sec", "JOBS_DLQ_METRIC_REFRESH_SEC", 10);
        // Per-registration "every type ever published" set, so a queue that
        // DRAINS gets reset to 0 — dlq_depth_by_type() omits empty types, so
        // without this the gauge for a now-empty type sticks at its last value.
        // Owned by the lambda (shared_ptr by value), so it's fresh on each
        // Core init and freed when Tasks drops the task — no cross-reinit leak
        // (a function-local static would have leaked one test's types into the
        // next in a long-lived test binary).
        auto ever_seen = std::make_shared<std::unordered_set<std::string>>();
        Tasks::schedule_recurring("jobs_dlq_depth_refresh", std::chrono::seconds(refresh_sec), [&family, ever_seen] {
            // `family` is owned by the Observability registry. Shutdown order
            // (Tasks before Observability) plus app().quit() before
            // Core::shutdown() means the timer is stopped while the loop is
            // already idle, so `family` outlives every tick. The guard is
            // defense-in-depth: if a tick ever raced teardown, bail before
            // touching the family rather than dereferencing a freed registry.
            if (!Observability::is_initialized() || !Jobs::is_initialized())
                return;
            auto per_type = Jobs::get().dlq_depth_by_type();
            long total = 0;
            for (const auto& [type, depth] : per_type) {
                family.Add({{"type", type}}).Set(static_cast<double>(depth));
                ever_seen->insert(type);
                total += depth;
            }
            for (const auto& type : *ever_seen) {
                if (per_type.find(type) == per_type.end())
                    family.Add({{"type", type}}).Set(0.0);
            }
            family.Add({{"type", "_total"}}).Set(static_cast<double>(total));
        });
    }

    // Registers jobs_queue_depth as a Prometheus gauge, labeled by job type
    // (special label type="_total" for the aggregate). The LEADING indicator
    // of saturation — a climbing waiting-queue means submitters are outrunning
    // the worker pool, visible long before anything lands in the DLQ. Mirrors
    // register_dlq_metric_ exactly, over jobs:queue:* instead of jobs:dlq:*.
    static void register_queue_depth_metric_(Config::AppConfig& cfg) {
        if (!Observability::is_initialized() || !Tasks::is_initialized())
            return;
        auto& family = Observability::get().metrics().create_gauge("jobs_queue_depth",
                                                                   "Current depth of the waiting jobs queue by type "
                                                                   "(special label type=\"_total\" for the aggregate)");
        int refresh_sec = cfg.get<int>("jobs.queue_metric_refresh_sec", "JOBS_QUEUE_METRIC_REFRESH_SEC", 10);
        // Same drain-to-zero bookkeeping as the DLQ gauge: queue_depth_by_type()
        // omits empty types, so a queue that drains would otherwise stick at its
        // last value.
        auto ever_seen = std::make_shared<std::unordered_set<std::string>>();
        Tasks::schedule_recurring("jobs_queue_depth_refresh", std::chrono::seconds(refresh_sec), [&family, ever_seen] {
            if (!Observability::is_initialized() || !Jobs::is_initialized())
                return;
            auto per_type = Jobs::get().queue_depth_by_type();
            long total = 0;
            for (const auto& [type, depth] : per_type) {
                family.Add({{"type", type}}).Set(static_cast<double>(depth));
                ever_seen->insert(type);
                total += depth;
            }
            for (const auto& type : *ever_seen) {
                if (per_type.find(type) == per_type.end())
                    family.Add({{"type", type}}).Set(0.0);
            }
            family.Add({{"type", "_total"}}).Set(static_cast<double>(total));
        });
    }

    // Registers db_pool_active_connections + db_pool_size gauges, labeled by
    // pool (primary/replica). Saturation = active / size → 1.0 means acquire()
    // is about to start timing out; it's the cause the HighP99Latency alert
    // tells operators to check first. Refreshed every N seconds (the counts
    // are cheap atomics — no DB round-trip).
    static void register_db_pool_metric_(Config::AppConfig& cfg) {
        if (!Observability::is_initialized() || !Tasks::is_initialized() || !Database::is_initialized())
            return;
        auto& active_family = Observability::get().metrics().create_gauge(
            "db_pool_active_connections", "In-use database connections by pool (saturation = this / db_pool_size)");
        auto& size_family = Observability::get().metrics().create_gauge(
            "db_pool_size", "Configured database connection pool size by pool");
        int refresh_sec = cfg.get<int>("database.pool_metric_refresh_sec", "DB_POOL_METRIC_REFRESH_SEC", 10);
        Tasks::schedule_recurring(
            "db_pool_metric_refresh", std::chrono::seconds(refresh_sec), [&active_family, &size_family] {
                if (!Observability::is_initialized() || !Database::is_initialized())
                    return;
                auto& db = Database::get();
                active_family.Add({{"pool", "primary"}}).Set(static_cast<double>(db.primary_pool_active()));
                size_family.Add({{"pool", "primary"}}).Set(static_cast<double>(db.primary_pool_size()));
                if (db.replica_count() > 0) {
                    active_family.Add({{"pool", "replica"}}).Set(static_cast<double>(db.replica_pool_active()));
                    size_family.Add({{"pool", "replica"}}).Set(static_cast<double>(db.replica_pool_size()));
                }
            });
    }

    // Registers db_replica_lag_seconds — how far a read replica trails the
    // primary, in seconds. A climbing value means read-after-write reads can
    // serve stale rows (the "I just saved it but it's gone" ghost); alert on it.
    // Only registered when replicas are configured — the primary has no replay
    // timestamp (the query returns NULL there). The round-robin read lands on
    // some replica each tick, so with several replicas the gauge reports
    // whichever it hit; that's enough to catch a lagging fleet.
    static void register_replication_lag_metric_(Config::AppConfig& cfg) {
        if (!Observability::is_initialized() || !Tasks::is_initialized() || !Database::is_initialized())
            return;
        if (Database::get().replica_count() == 0)
            return;
        auto& family = Observability::get().metrics().create_gauge(
            "db_replica_lag_seconds", "Read-replica replication lag behind the primary, in seconds");
        int refresh_sec =
            cfg.get<int>("database.replica_lag_metric_refresh_sec", "DB_REPLICA_LAG_METRIC_REFRESH_SEC", 15);
        Tasks::schedule_recurring("db_replica_lag_refresh", std::chrono::seconds(refresh_sec), [&family] {
            if (!Observability::is_initialized() || !Database::is_initialized())
                return;
            try {
                auto lag = Database::get().execute_read([](auto& txn) -> std::optional<double> {
                    auto r =
                        txn.exec("SELECT EXTRACT(EPOCH FROM (now() - pg_last_xact_replay_timestamp()))::float8 AS lag");
                    if (r.empty() || r[0]["lag"].is_null())
                        return std::nullopt;  // landed on the primary / nothing replayed yet
                    return r[0]["lag"].template as<double>();
                });
                if (lag)
                    family.Add({}).Set(std::max(0.0, *lag));  // clamp tiny clock-skew negatives
            } catch (const std::exception& e) {
                spdlog::warn("db_replica_lag refresh failed: {}", e.what());
            }
        });
    }

    // Periodically prune expired single-use token nonces (used_tokens,
    // migration 002). Unlike the old Redis TTL nonce these rows are permanent,
    // so without a reaper the table + its index grow monotonically.
    static void register_token_reaper_() {
        if (!Tasks::is_initialized() || !Database::is_initialized())
            return;
        Tasks::schedule_recurring("used_tokens_reaper", std::chrono::hours(1), [] {
            if (!Database::is_initialized())
                return;
            try {
                Database::get().execute_write([](auto& txn) {
                    txn.exec("DELETE FROM used_tokens WHERE expires_at < now()");
                    return 0;
                });
            } catch (const std::exception& e) {
                spdlog::warn("used_tokens reaper failed: {}", e.what());
            }
        });
    }

    // Registers the subsystem probes the template ships with. Services
    // that add their own modules can call Core::get().register_health_check
    // at any point after Core::initialize() returns.
    void register_default_health_checks_() {
        if (Database::is_initialized()) {
            register_health_check("database", [] { return Database::get().health_check(); });
        }
        if (Cache::is_initialized()) {
            register_health_check("cache", [] { return Cache::get().health_check(); });
        }
        if (Jobs::is_initialized()) {
            register_health_check("jobs", [] { return Jobs::get().health_check(); });
        }
        // Optional dependencies (SMTP, object storage, Kafka) belong here as
        // DEGRADED probes — their outage should show in /health but must NOT
        // pull the pod out of rotation via /ready. Register them once those
        // modules expose a cheap connectivity check, e.g.:
        //   if (Messaging::is_initialized())
        //       register_health_check("kafka", [] { return Messaging::get().health_check(); }, /*critical=*/false);
    }

    static void init_jobs_(Config::AppConfig& cfg) {
        if (!cfg.get<bool>("jobs.enabled", "JOBS_ENABLED", false))
            return;
        long ttl = cfg.get<int>("jobs.result_ttl", "JOBS_RESULT_TTL", 86400);
        int retries = cfg.get<int>("jobs.max_retries", "JOBS_MAX_RETRIES", 3);
        Jobs::initialize(ttl, retries);
        // Reliability knobs (opt-in; 0 keeps the legacy immediate-requeue /
        // no-lease behaviour). promote_due_jobs() / reap_expired_leases() are
        // driven from the worker loop.
        const int backoff_base = cfg.get<int>("jobs.retry_backoff_base_ms", "JOBS_RETRY_BACKOFF_BASE_MS", 0);
        const int backoff_max = cfg.get<int>("jobs.retry_backoff_max_ms", "JOBS_RETRY_BACKOFF_MAX_MS", 60000);
        const int visibility = cfg.get<int>("jobs.visibility_timeout_sec", "JOBS_VISIBILITY_TIMEOUT_SEC", 0);
        Jobs::get().set_retry_backoff(backoff_base, backoff_max);
        Jobs::get().set_visibility_timeout(visibility);
        register_dlq_metric_(cfg);
        register_queue_depth_metric_(cfg);
    }

public:
    using HealthFn = std::function<bool()>;

    /**
     * @brief Register a health probe. The passed callable is invoked each
     *        time /health or /ready rolls through the list.
     * @param critical When true (default) a failing probe makes /ready report
     *        NotReady (kube pulls the pod from rotation). When false the probe
     *        is "degraded": still surfaced in /health, but a failure does NOT
     *        fail readiness — for optional dependencies (SMTP, object storage,
     *        Kafka) whose outage shouldn't take the whole service out of
     *        rotation. Thread-safe; typically called once during subsystem init.
     */
    void register_health_check(std::string name, HealthFn probe, bool critical = true) {
        std::lock_guard<std::mutex> lock(health_mu_);
        health_checks_.push_back({std::move(name), std::move(probe), critical});
    }

    struct ComponentHealth {
        std::string name;
        bool initialized;
        bool healthy;
        bool critical;
    };

    /**
     * @brief Report every registered probe's current state. Used by
     *        /health to produce the per-component breakdown.
     */
    std::vector<ComponentHealth> health_report() {
        std::vector<ComponentHealth> report;
        std::lock_guard<std::mutex> lock(health_mu_);
        for (const auto& e : health_checks_) {
            bool ok = false;
            try {
                ok = e.probe();
            } catch (...) {
                ok = false;
            }
            report.push_back({e.name, true, ok, e.critical});
        }
        return report;
    }

    /**
     * @brief True iff every CRITICAL probe currently passes. Degraded
     *        (non-critical) probes are ignored — they show up in /health but
     *        never fail readiness. No `initialized` guard, so it's unit-testable
     *        directly; health_check() adds that guard for the live /ready path.
     */
    bool all_critical_healthy() {
        std::lock_guard<std::mutex> lock(health_mu_);
        for (const auto& e : health_checks_) {
            if (!e.critical)
                continue;
            try {
                if (!e.probe()) {
                    spdlog::warn("{} health check failed", e.name);
                    return false;
                }
            } catch (const std::exception& ex) {
                spdlog::warn("{} health check threw: {}", e.name, ex.what());
                return false;
            }
        }
        return true;
    }

    bool health_check() { return initialized && all_critical_healthy(); }

private:
    struct HealthEntry {
        std::string name;
        HealthFn probe;
        bool critical = true;
    };
    std::vector<HealthEntry> health_checks_;
    std::mutex health_mu_;

public:
    void shutdown() {
        // No early return on !initialized: initialize() calls shutdown() from
        // its catch block to unwind a PARTIAL init (e.g. Cache threw while
        // Database/Observability/Config were already up). Each module's
        // shutdown() is a no-op when its global is null, so running them all
        // unconditionally is safe and leaves nothing dangling for a retry.
        spdlog::info("=== Application Shutdown Started ===");

        if (Email::is_initialized())
            Email::shutdown();
        if (Jobs::is_initialized())
            Jobs::shutdown();
        if (Security::Idempotency::is_initialized())
            Security::Idempotency::shutdown();
        if (Security::RateLimit::is_initialized())
            Security::RateLimit::shutdown();
        if (Security::Auth::is_initialized())
            Security::Auth::shutdown();
        if (Tasks::is_initialized())
            Tasks::shutdown();
        if (Messaging::is_initialized())
            Messaging::shutdown();
        if (Cache::is_initialized())
            Cache::shutdown();
        if (Migrations::is_initialized())
            Migrations::shutdown();
        if (Database::is_initialized())
            Database::shutdown();

        if (Observability::is_initialized()) {
            spdlog::info("=== Application Shutdown Complete ===");
            // Unbind metric sinks before tearing down the registry they
            // point at — events after this would otherwise dereference a
            // freed Prometheus family.
            Retry::reset_metrics();
            Database::reset_query_metrics();
            Observability::shutdown();
        }

        if (Config::is_initialized())
            Config::shutdown();

        initialized = false;
    }

    bool is_initialized() const { return initialized; }

    void reload_config() {
        if (!initialized) {
            throw std::runtime_error("Cannot reload config: application not initialized");
        }
        spdlog::info("Reloading configuration from: {}", config_file);
        Config::get().reload();
        spdlog::info("Configuration reloaded successfully");
    }

    // Generated from project(VERSION ...) in CMakeLists.txt — the single
    // place the version number lives.
    std::string version() const { return CPP_API_VERSION; }
};

/**
 * @brief Global application instance
 */
inline std::unique_ptr<Application> global_app = nullptr;

inline void initialize(const std::string& config_path, InitMode mode = InitMode::Full) {
    if (global_app != nullptr) {
        throw std::runtime_error("Application already initialized");
    }
    global_app = std::make_unique<Application>();
    try {
        global_app->initialize(config_path, mode);
    } catch (...) {
        // Leave no dangling global on failure so the next call can try again.
        global_app.reset();
        throw;
    }
}

inline Application& get() {
    if (global_app == nullptr) {
        throw std::runtime_error("Application not initialized");
    }
    return *global_app;
}

inline bool is_initialized() {
    return global_app != nullptr && global_app->is_initialized();
}

inline void shutdown() {
    if (global_app) {
        global_app->shutdown();
        global_app.reset();
    }
}

inline bool health_check() {
    if (!is_initialized())
        return false;
    return global_app->health_check();
}

/**
 * @brief Shutdown state flag, flipped by main/worker signal handlers before
 *        Core::shutdown() is called. Used by /ready so Kubernetes stops
 *        sending new traffic while in-flight requests complete.
 */
inline std::atomic<bool> shutting_down_flag{false};

inline void begin_shutdown() {
    shutting_down_flag.store(true);
}
inline bool is_shutting_down() {
    return shutting_down_flag.load();
}

}  // namespace Core
