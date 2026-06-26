/**
 * @file main.cpp
 * @brief Application entry point
 * @details Parses CLI/ops flags, then either runs a one-shot mode
 *          (migrations, config dump, admin bootstrap, …) or boots the
 *          Drogon HTTP server. Each mode lives in its own function; main()
 *          is just argument parsing + dispatch.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include <drogon/drogon.h>

#include "api/Api.hpp"
#include "core/Core.hpp"
#include "database/Database.hpp"
#include "database/Migrations.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Password.hpp"
#include "utils/Config.hpp"

namespace {

// Signal-set flag observed by the shutdown-monitor thread. sig_atomic-ish
// semantics are sufficient; we only ever flip false → true from the handler.
std::atomic<bool> shutdown_requested{false};

void signal_handler(int /*signal*/) {
    shutdown_requested.store(true);
}

// Parsed command line. `mode` is the ops flag (empty = boot the server);
// arg1/arg2 are mode-specific positional data (e.g. email + password).
struct CliArgs {
    std::string mode;
    std::string config_file = "config/config.json";
    std::string arg1;
    std::string arg2;
};

CliArgs parse_cli(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            args.mode = "--help";
            break;
        }
        if (a == "--print-routes" || a == "--dump-config" || a == "--verify-migrations" || a == "--run-migrations" ||
            a == "--setup-dev") {
            args.mode = a;
            continue;
        }
        // Modes that consume tail arguments. create-admin takes EMAIL [PASS],
        // seed-fake takes [N]. A positional config path must precede the flag.
        if (a == "--create-admin") {
            args.mode = a;
            if (i + 1 < argc)
                args.arg1 = argv[++i];
            if (i + 1 < argc)
                args.arg2 = argv[++i];
            continue;
        }
        if (a == "--seed-fake") {
            args.mode = a;
            if (i + 1 < argc)
                args.arg1 = argv[++i];
            continue;
        }
        // Positional: first non-flag arg is the config path.
        args.config_file = a;
    }
    const char* config_env = std::getenv("CONFIG_FILE");
    if (config_env != nullptr)
        args.config_file = config_env;
    return args;
}

void print_usage() {
    std::cout << "Usage: tarassov_me [config.json] [ops flag]\n"
              << "  --print-routes              print registered endpoints and exit\n"
              << "  --dump-config               print resolved config as JSON and exit\n"
              << "  --verify-migrations         list pending migrations and exit (1 if any)\n"
              << "  --run-migrations            apply pending migrations and exit\n"
              << "  --setup-dev                 apply migrations + seed roles (no-op if already done)\n"
              << "  --create-admin EMAIL [PASS] create an Administrator user (default password: 'password')\n"
              << "  --seed-fake [N]             insert N fake users (default 10) for dev/demo\n"
              << "  --help, -h                  this message\n"
              << "\nPositional: path to config JSON (default: config/config.json)\n";
}

void print_routes() {
    for (const auto& ep : Api::get_endpoints()) {
        std::cout << std::left << std::setw(7) << ep.method << std::setw(32) << ep.path << ep.description << "\n";
    }
}

// Migrate-only mode (for Helm init-container or ad-hoc migration runs).
int run_migrate_only(const std::string& config_file) {
    std::cout << "RUN_MIGRATIONS_ONLY=true — running migrations and exiting" << std::endl;
    Core::initialize(config_file, Core::InitMode::MigrateOnly);
    Core::shutdown();
    std::cout << "Migrations complete" << std::endl;
    return 0;
}

// Resolve the config (JSON + env overrides) and print it. Keeps
// Observability/DB/etc. offline — safe to run against a production config
// file without side effects.
int run_dump_config(const std::string& config_file) {
    Config::initialize(config_file);
    std::cout << Config::get().get_json().dump(2) << std::endl;
    Config::shutdown();
    return 0;
}

// Bring up Config + Database just far enough to query schema_migrations, then
// report pending files without applying anything. Exits 1 if anything is
// pending — useful in CI gates that want to catch "deployed app vs. DB" drift.
int run_verify_migrations(const std::string& config_file) {
    Config::initialize(config_file);
    auto& cfg = Config::get();
    auto primary = cfg.get<std::string>("database.primary", "DATABASE_PRIMARY_URL", "postgresql://localhost:5432/app");
    int pool_size = cfg.get<int>("database.pool_size", "DB_POOL_SIZE", 10);
    int acquire_ms = cfg.get<int>("database.acquire_timeout_ms", "DB_ACQUIRE_TIMEOUT_MS", 5000);
    Database::initialize(primary, {}, pool_size, std::chrono::milliseconds(acquire_ms));
    auto dir = cfg.get<std::string>("database.migrations_dir", "DB_MIGRATIONS_DIR", "migrations");
    auto pending = Migrations::MigrationRunner::list_pending(dir);
    if (pending.empty()) {
        std::cout << "All migrations applied." << std::endl;
    } else {
        std::cout << "Pending migrations:" << std::endl;
        for (const auto& m : pending) {
            std::cout << "  " << m.version << "  " << m.name << std::endl;
        }
    }
    Database::shutdown();
    Config::shutdown();
    return pending.empty() ? 0 : 1;
}

// Run migrations (seeding the starter roles) then exit. Idempotent.
// flask-base parity: manage.py setup_dev.
int run_setup_dev(const std::string& config_file) {
    Core::initialize(config_file, Core::InitMode::MigrateOnly);
    std::cout << "setup-dev: migrations applied; roles seeded." << std::endl;
    Core::shutdown();
    return 0;
}

// Create an Administrator user, or upgrade the existing one to Administrator +
// reset its password. flask-base parity: manage.py setup_general ADMIN_EMAIL.
int run_create_admin(const std::string& config_file, const std::string& email, const std::string& password) {
    if (email.empty()) {
        std::cerr << "ERROR: --create-admin requires EMAIL [PASSWORD]" << std::endl;
        return 2;
    }
    Core::initialize(config_file);
    Repositories::RoleRepository roles;
    auto admin_role = roles.find_by_name("Administrator");
    if (!admin_role) {
        std::cerr << "ERROR: Administrator role missing — run --setup-dev first" << std::endl;
        Core::shutdown();
        return 3;
    }
    Repositories::UserRepository users;
    auto existing = users.find_by_email(email);
    const std::string hash = Security::Password::hash(password);
    if (existing) {
        users.update_password_hash(existing->id, hash);
        users.change_role(existing->id, admin_role->id);
        std::cout << "Updated existing user " << email << " — role=Administrator, password reset." << std::endl;
    } else {
        auto created = users.create(email, hash, std::nullopt, std::nullopt, admin_role->id, /*confirmed=*/true);
        std::cout << "Created Administrator " << email << " (id=" << created.id << ")" << std::endl;
    }
    Core::shutdown();
    return 0;
}

// Insert N (default 10) fake users for dev/demo. flask-base parity:
// manage.py add_fake_data — deterministic pattern instead of a Faker dep.
int run_seed_fake(const std::string& config_file, const std::string& count_arg) {
    int n = 10;
    if (!count_arg.empty()) {
        try {
            n = std::stoi(count_arg);
        } catch (...) {
            n = 10;
        }
    }
    if (n < 1)
        n = 1;
    if (n > 1000)
        n = 1000;
    Core::initialize(config_file);
    Repositories::RoleRepository roles;
    auto user_role = roles.find_default();
    if (!user_role) {
        std::cerr << "ERROR: default role missing — run --setup-dev first" << std::endl;
        Core::shutdown();
        return 3;
    }
    Repositories::UserRepository users;
    const std::string hash = Security::Password::hash("password");
    int created = 0;
    for (int i = 0; i < n; ++i) {
        std::string email = "fake" + std::to_string(i) + "@example.com";
        try {
            users.create(email,
                         hash,
                         std::string("Fake"),
                         std::string(std::to_string(i)),
                         user_role->id,
                         /*confirmed=*/true);
            ++created;
        } catch (const Repositories::DuplicateEmail&) {
            // already there from a previous run — skip silently
        }
    }
    std::cout << "Seeded " << created << " fake user(s) (skipped " << (n - created) << " already-existing)."
              << std::endl;
    Core::shutdown();
    return 0;
}

// Boot the HTTP server and block until a shutdown signal drains it.
int run_server(const std::string& config_file) {
    Core::initialize(config_file);
    auto& config = Config::get();

    std::string host = config.get<std::string>("server.host", "SERVER_HOST", "0.0.0.0");
    int port = config.get<int>("server.port", "SERVER_PORT", 8080);
    int threads = config.get<int>("server.threads", "SERVER_THREADS", 0);
    if (threads <= 0) {
        // 0 / unset = auto: one IO thread per core. Under the synchronous pqxx
        // model, in-flight DB calls are capped by the THREAD count — not by
        // database.pool_size — so this is the real concurrency knob.
        const unsigned hc = std::thread::hardware_concurrency();
        threads = static_cast<int>(hc > 0 ? hc : 4);
    }
    // Effective DB concurrency is min(threads, pool). A pool smaller than the
    // thread count makes threads queue on acquire() (latency spikes that look
    // like a slow DB); a pool much larger leaves the extra connections inert and
    // makes the db_pool saturation gauge under-report. Warn so they're tuned
    // together; see docs/CONFIG.md.
    {
        const int db_pool = config.get<int>("database.pool_size", "DB_POOL_SIZE", 10);
        if (db_pool < threads) {
            spdlog::warn(
                "server.threads={} exceeds database.pool_size={}: IO threads will contend for DB "
                "connections (acquire stalls under load). Raise database.pool_size to >= threads.",
                threads,
                db_pool);
        }
    }
    bool enable_ssl = config.get<bool>("server.ssl.enabled", "SERVER_SSL_ENABLED", false);
    // Cap request body to keep a single client from arbitraging memory.
    // 10 MB is generous for JSON APIs; bump for file uploads and configure
    // proxies (nginx client_max_body_size, etc.) accordingly.
    long max_body_bytes = config.get<long>("server.max_body_bytes", "SERVER_MAX_BODY_BYTES", 10L * 1024 * 1024);
    // Grace period between "readiness goes 503" and "Drogon stops accepting".
    // Give Kubernetes time to propagate the endpoint removal to all kube-proxy
    // instances so no new traffic lands on the pod during shutdown.
    int pre_stop_delay_sec = config.get<int>("shutdown.pre_stop_delay_sec", "SHUTDOWN_PRE_STOP_DELAY_SEC", 5);

    drogon::app()
        .addListener(host, port)
        .setThreadNum(threads)
        .setClientMaxBodySize(static_cast<size_t>(max_body_bytes))
        .setLogLevel(trantor::Logger::kInfo)
        .setLogPath("./logs");

    if (enable_ssl) {
        std::string cert_file = config.get<std::string>("server.ssl.cert", "SSL_CERT_FILE", "");
        std::string key_file = config.get<std::string>("server.ssl.key", "SSL_KEY_FILE", "");
        if (!cert_file.empty() && !key_file.empty()) {
            drogon::app().setSSLFiles(cert_file, key_file);
            std::cout << "SSL enabled" << std::endl;
        }
    }

    Api::register_controllers();

    std::cout << "==================================================" << std::endl;
    std::cout << "Server starting on " << host << ":" << port << std::endl;
    std::cout << "Threads: " << threads << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "\nEndpoints:" << std::endl;
    for (const auto& ep : Api::get_endpoints()) {
        std::cout << "  " << std::left << std::setw(7) << ep.method << std::setw(26) << ep.path << ep.description
                  << std::endl;
    }
    std::cout << "  Metrics: http://" << host << ":9090/metrics" << std::endl;
    std::cout << "\nPress Ctrl+C to stop the server\n" << std::endl;

    // Shutdown monitor: watches the signal flag and orchestrates the drain.
    //   1. Flip readiness to 503 so kube-proxy stops sending new requests.
    //   2. Sleep pre_stop_delay_sec so endpoint controller propagates the
    //      503 before we stop accepting.
    //   3. Call Drogon quit() to return from run() on the main thread.
    std::thread shutdown_monitor([pre_stop_delay_sec] {
        while (!shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        Core::begin_shutdown();
        spdlog::info(
            "SIGTERM received — readiness now 503, waiting {}s "
            "before stopping Drogon",
            pre_stop_delay_sec);
        std::this_thread::sleep_for(std::chrono::seconds(pre_stop_delay_sec));
        spdlog::info("Stopping Drogon event loop");
        drogon::app().quit();
    });

    // Blocks until quit() is called (from the shutdown monitor).
    drogon::app().run();

    // Ensure the monitor exits even if Drogon returned for other reasons.
    shutdown_requested.store(true);
    if (shutdown_monitor.joinable())
        shutdown_monitor.join();

    std::cout << "Server stopped, tearing down subsystems" << std::endl;
    Core::shutdown();

    std::cout << "Application exited successfully" << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        // Ignore SIGPIPE — broken client sockets should surface as EPIPE from
        // writes, not kill the process.
        std::signal(SIGPIPE, SIG_IGN);

        const CliArgs args = parse_cli(argc, argv);

        // Flags that need neither banner nor Core init.
        if (args.mode == "--help") {
            print_usage();
            return 0;
        }
        if (args.mode == "--print-routes") {
            print_routes();
            return 0;
        }

        std::cout << "==================================================" << std::endl;
        std::cout << "       C++ API Template Starting                  " << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "Loading configuration from: " << args.config_file << std::endl;
        std::filesystem::create_directories("logs");

        // Migrate-only mode (CLI flag or RUN_MIGRATIONS_ONLY env — equivalent).
        const char* migrate_only_env = std::getenv("RUN_MIGRATIONS_ONLY");
        const bool migrate_only = args.mode == "--run-migrations" ||
                                  (migrate_only_env != nullptr &&
                                   (std::string(migrate_only_env) == "true" || std::string(migrate_only_env) == "1"));
        if (migrate_only)
            return run_migrate_only(args.config_file);

        if (args.mode == "--dump-config")
            return run_dump_config(args.config_file);
        if (args.mode == "--verify-migrations")
            return run_verify_migrations(args.config_file);

        // CLI one-shots are typically exec'd inside a container that already
        // has the long-running server bound to METRICS_ADDRESS. Disable the
        // exposer so the second process doesn't try to bind the same port.
        if (args.mode == "--setup-dev" || args.mode == "--create-admin" || args.mode == "--seed-fake") {
            ::setenv("METRICS_ADDRESS", "", /*overwrite=*/1);
        }

        if (args.mode == "--setup-dev")
            return run_setup_dev(args.config_file);
        if (args.mode == "--create-admin")
            return run_create_admin(args.config_file, args.arg1, args.arg2.empty() ? "password" : args.arg2);
        if (args.mode == "--seed-fake")
            return run_seed_fake(args.config_file, args.arg1);

        return run_server(args.config_file);

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        try {
            Core::shutdown();
        } catch (...) {}
        return 1;
    }
}
