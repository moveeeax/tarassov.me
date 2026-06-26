/**
 * @file worker_main.cpp
 * @brief Worker process entry point
 * @details Runs background job processing via Redis BRPOP loop.
 *          Exposes a lightweight Drogon health endpoint on a separate port.
 */

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <drogon/drogon.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "core/Core.hpp"
#include "email/AccountEmailWorker.hpp"
#include "jobs/BuiltinHandlers.hpp"
#include "jobs/Dispatcher.hpp"
#include "jobs/Jobs.hpp"
#include "observability/Observability.hpp"
#include "observability/Trace.hpp"
#include "utils/Config.hpp"
#include "utils/Strings.hpp"

using json = nlohmann::json;

// Global shutdown flag (observed by worker threads and the /ready handler).
static std::atomic<bool> shutdown_requested{false};

// Prometheus families created once in main() after Observability is up.
// The worker loop increments / observes them on every pick-process cycle.
namespace worker_metrics {
inline prometheus::Family<prometheus::Counter>* processed_total = nullptr;
inline prometheus::Family<prometheus::Histogram>* processing_duration = nullptr;
inline prometheus::Family<prometheus::Gauge>* active_jobs = nullptr;
inline const std::vector<double> DURATION_BUCKETS = {
    0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0};
}  // namespace worker_metrics

// Only the atomic store happens here. Core::begin_shutdown() and
// drogon::app().quit() take locks / touch the event loop — neither is
// async-signal-safe — so a monitor thread in main() watches the flag and
// performs the actual drain (same pattern as main.cpp).
void signal_handler(int /*signal*/) {
    shutdown_requested.store(true);
}

// register_builtin_handlers() lives in jobs/BuiltinHandlers.hpp so it's
// unit-testable (see tests/unit/test_job_dispatch.cpp).

/**
 * @brief Process a single job by dispatching on its type. Unknown types throw
 *        Jobs::PermanentJobError (→ straight to DLQ) from the Dispatcher.
 */
json process_job(const Jobs::Job& job) {
    return Jobs::Dispatcher::get().dispatch(job);
}

namespace {

bool hex_to_bytes(std::string_view hex, uint8_t* out, size_t n) {
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
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// Rebuild the originating request's span context from the W3C traceparent the
// submitter stamped on the job, so the worker's processing span continues the
// same distributed trace instead of starting an unrelated root.
std::optional<opentelemetry::trace::SpanContext> parent_ctx_from_traceparent(const std::string& tp) {
    if (tp.empty())
        return std::nullopt;
    auto parsed = Observability::Trace::parse_traceparent(tp);
    if (!parsed)
        return std::nullopt;
    uint8_t tid[16], sid[8], flags[1];
    if (!hex_to_bytes(parsed->trace_id, tid, 16) || !hex_to_bytes(parsed->parent_id, sid, 8) ||
        !hex_to_bytes(parsed->flags, flags, 1))
        return std::nullopt;
    return opentelemetry::trace::SpanContext(
        opentelemetry::trace::TraceId(opentelemetry::nostd::span<const uint8_t, 16>(tid)),
        opentelemetry::trace::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(sid)),
        opentelemetry::trace::TraceFlags(flags[0]),
        /*is_remote=*/true);
}

}  // namespace

/**
 * @brief Worker thread loop: BRPOP → process → complete/fail
 */
void worker_loop(const std::string& worker_id, const std::vector<std::string>& types, long brpop_timeout) {
    {
        std::string types_joined;
        for (size_t i = 0; i < types.size(); ++i) {
            if (i > 0)
                types_joined += ",";
            types_joined += types[i];
        }
        spdlog::info("Worker thread started: id={} types={}", worker_id, types_joined);
    }

    while (!shutdown_requested.load()) {
        try {
            // Maintenance: promote backoff-delayed jobs that are due and reclaim
            // jobs whose visibility lease expired (a worker that died mid-job).
            // Both are cheap no-ops unless enabled via config; idempotent across
            // worker threads (ZREM-gated), so running them on every thread/cycle
            // is safe — worst case is a wasted ZRANGEBYSCORE on an empty set.
            try {
                Jobs::get().promote_due_jobs();
                Jobs::get().reap_expired_leases();
            } catch (...) {}

            auto job = Jobs::get().pick(types, brpop_timeout, worker_id);
            if (!job)
                continue;

            spdlog::info("Processing job: id={} type={}", job->id, job->type);

            // Create a trace span for the job. If the submitter stamped a
            // traceparent, continue that distributed trace (child span);
            // otherwise this is a fresh root.
            auto tracer = Observability::get().tracer().get_tracer("worker");
            opentelemetry::trace::StartSpanOptions span_opts;
            if (auto parent = parent_ctx_from_traceparent(job->traceparent))
                span_opts.parent = *parent;
            auto span = tracer->StartSpan("job.process " + job->type,
                                          {{"job.id", job->id}, {"job.type", job->type}, {"worker.id", worker_id}},
                                          span_opts);
            opentelemetry::trace::Scope scope(span);

            // Persist the trace id on the job blob so the admin UI can
            // deep-link into Jaeger. Best-effort: listing works without it.
            try {
                char trace_hex[32];
                span->GetContext().trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(trace_hex, 32));
                Jobs::get().set_trace_id(job->id, std::string(trace_hex, 32));
            } catch (const std::exception& e) {
                spdlog::debug("set_trace_id failed for job {}: {}", job->id, e.what());
            }

            // Metrics: active_jobs gauge up for the duration of processing,
            // decremented by an RAII guard so it can't get stuck +1 if
            // complete()/fail()/dead_letter() throws (a wedged-looking worker).
            // Histogram + outcome counter are recorded by record_done.
            struct ActiveJobGuard {
                std::string type;
                bool armed = false;
                ~ActiveJobGuard() {
                    if (armed && worker_metrics::active_jobs)
                        worker_metrics::active_jobs->Add({{"type", type}}).Decrement();
                }
            } active_guard{job->type};
            if (worker_metrics::active_jobs) {
                worker_metrics::active_jobs->Add({{"type", job->type}}).Increment();
                active_guard.armed = true;
            }
            const auto started = std::chrono::steady_clock::now();
            auto record_done = [&](const char* outcome) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                if (worker_metrics::processing_duration) {
                    worker_metrics::processing_duration->Add({{"type", job->type}}, worker_metrics::DURATION_BUCKETS)
                        .Observe(elapsed);
                }
                if (worker_metrics::processed_total) {
                    worker_metrics::processed_total->Add({{"type", job->type}, {"outcome", outcome}}).Increment();
                }
            };

            try {
                auto result = process_job(*job);
                Jobs::get().complete(job->id, result);
                span->SetStatus(opentelemetry::trace::StatusCode::kOk);
                span->End();
                record_done("success");
                spdlog::info("Job completed: id={}", job->id);
            } catch (const Jobs::PermanentJobError& e) {
                // No retries — straight to DLQ so a permanent mismatch can't
                // spin the worker through its full retry budget.
                spdlog::error("Job permanently failed (no retry): id={} error={}", job->id, e.what());
                span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
                span->SetAttribute("error.message", std::string(e.what()));
                span->End();
                Jobs::get().dead_letter(job->id, e.what());
                record_done("permanent_failure");
            } catch (const std::exception& e) {
                spdlog::error("Job failed: id={} error={}", job->id, e.what());
                span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
                span->SetAttribute("error.message", std::string(e.what()));
                span->End();
                Jobs::get().fail(job->id, e.what());
                record_done("failure");
            } catch (...) {
                // A handler throwing a non-std type must not std::terminate the
                // worker thread (Dispatcher invites arbitrary handlers).
                spdlog::error("Job failed with a non-std exception: id={}", job->id);
                span->SetStatus(opentelemetry::trace::StatusCode::kError, "non-std exception");
                span->End();
                Jobs::get().fail(job->id, "non-std exception in handler");
                record_done("failure");
            }
        } catch (const std::exception& e) {
            if (shutdown_requested.load())
                break;
            spdlog::error("Worker loop error: {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    spdlog::info("Worker thread stopped: id={}", worker_id);
}

int main(int argc, char* argv[]) {
    try {
        // Signal handlers
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::cout << "==================================================" << std::endl;
        std::cout << "       C++ API Worker Starting                     " << std::endl;
        std::cout << "==================================================" << std::endl;

        // Config file path
        std::string config_file = "config/config.json";
        if (argc > 1) {
            config_file = argv[1];
        }
        const char* config_env = std::getenv("CONFIG_FILE");
        if (config_env != nullptr) {
            config_file = config_env;
        }

        std::cout << "Loading configuration from: " << config_file << std::endl;
        std::filesystem::create_directories("logs");

        // Initialize core in Worker mode
        Core::initialize(config_file, Core::InitMode::Worker);

        // Verify Jobs is enabled
        if (!Jobs::is_initialized()) {
            spdlog::error("JOBS_ENABLED is not set to true — worker cannot start");
            Core::shutdown();
            return 1;
        }

        // Register job-type handlers into the Dispatcher before any worker
        // thread can pick + dispatch.
        Jobs::register_builtin_handlers();

        // Rescue any jobs that the previous instance with this WORKER_ID
        // left in the processing list before crashing. Must run BEFORE
        // workers start picking, otherwise a thread could double-process
        // a job that's about to be put back on the live queue.
        {
            const std::string base_id = Config::get().get<std::string>("worker.id", "WORKER_ID", "worker-1");
            // Worker threads pick under "<base>-<n>" (see the spawn loop below),
            // so recovery must sweep each per-thread processing list. Recovering
            // only the bare base id (the old behaviour) matched nothing and
            // silently stranded in-flight jobs on crash. Same config keys/
            // defaults as the spawn loop so the ids line up. Also sweep the bare
            // id for back-compat with jobs left by an older single-id scheme.
            const int recover_concurrency = Config::get().get<int>("worker.concurrency", "WORKER_CONCURRENCY", 2);
            Jobs::get().recover_processing(base_id);
            for (int i = 0; i < recover_concurrency; ++i)
                Jobs::get().recover_processing(base_id + "-" + std::to_string(i));
        }

        // Register worker-specific Prometheus families. Labeled by job type
        // and (for processed_total) outcome.
        {
            auto& metrics = Observability::get().metrics();
            worker_metrics::processed_total = &metrics.create_counter(
                "jobs_processed_total", "Jobs processed by the worker, labeled by type and outcome (success|failure)");
            worker_metrics::processing_duration = &metrics.create_histogram(
                "job_processing_duration_seconds", "Wall-clock processing time per job, labeled by type");
            worker_metrics::active_jobs = &metrics.create_gauge(
                "worker_active_jobs", "Jobs currently being processed by this worker, labeled by type");
        }

        // Read worker configuration from env vars (override config.json)
        auto& config = Config::get();

        std::string worker_id = config.get<std::string>("worker.id", "WORKER_ID", "worker-1");
        int concurrency = config.get<int>("worker.concurrency", "WORKER_CONCURRENCY", 2);

        // Parse worker queue types (comma-separated env var or JSON array)
        std::vector<std::string> worker_types =
            Utils::Strings::split_csv_vec(config.get<std::string>("", "WORKER_TYPES", ""));
        if (worker_types.empty()) {
            // Fallback: read from config JSON array
            try {
                const auto& cfg = config.get_json();
                if (cfg.contains("worker") && cfg["worker"].contains("types")) {
                    for (const auto& t : cfg["worker"]["types"]) {
                        worker_types.push_back(t.get<std::string>());
                    }
                }
            } catch (...) {}
        }
        if (worker_types.empty()) {
            // Fall back to every registered handler type (not a hardcoded
            // "default" pseudo-queue that has no handler — which the guard rail
            // below would then flag on every boot). register_builtin_handlers()
            // ran above, so this is the real set this binary can process.
            worker_types = Jobs::Dispatcher::get().known_types();
        }

        // Guard rail: a WORKER_TYPES entry with no registered handler silently
        // dead-letters every job of that type (Dispatcher throws PermanentJobError
        // → straight to DLQ). Surface the producer/consumer mismatch at startup.
        // WORKER_STRICT_TYPES=true upgrades the warning to a hard refusal to start.
        if (auto missing = Jobs::Dispatcher::get().unregistered(worker_types); !missing.empty()) {
            std::string joined;
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i)
                    joined += ",";
                joined += missing[i];
            }
            if (config.get<bool>("worker.strict_types", "WORKER_STRICT_TYPES", false)) {
                spdlog::error(
                    "Worker subscribed to job type(s) with no handler: {} — refusing to start "
                    "(WORKER_STRICT_TYPES=true). Register a handler or fix WORKER_TYPES.",
                    joined);
                Core::shutdown();  // tear down subsystems + flush logs, like the other refuse-to-start paths
                return 1;
            }
            spdlog::warn(
                "Worker subscribed to job type(s) with NO registered handler: {} — jobs of these "
                "types will dead-letter on arrival. Register a handler or fix WORKER_TYPES "
                "(set WORKER_STRICT_TYPES=true to fail fast).",
                joined);
        }

        int health_port = config.get<int>("worker.health_port", "WORKER_HEALTH_PORT", 9091);
        long brpop_timeout = config.get<int>("worker.brpop_timeout", "WORKER_BRPOP_TIMEOUT", 5);

        // Create a dedicated Redis client for BRPOP with an appropriate socket_timeout.
        // The shared Cache pool's socket_timeout (default 500ms) is far below
        // brpop_timeout, so blocking ops would trip the socket first.
        {
            std::string redis_password = config.get<std::string>("cache.password", "REDIS_PASSWORD", "");
            std::string sentinel_password =
                config.get<std::string>("cache.sentinel.password", "REDIS_SENTINEL_PASSWORD", redis_password);
            bool use_sentinel = config.get<bool>("cache.use_sentinel", "REDIS_USE_SENTINEL", false);

            if (use_sentinel) {
                // Use Sentinel to discover the master — avoids connecting to a read replica.
                // Env var wins over JSON config so container deployments can override the
                // dev defaults in config/worker.json (which point at localhost).
                std::string master_name =
                    config.get<std::string>("cache.sentinel.master_name", "REDIS_MASTER_NAME", "mymaster");
                std::string sentinel_nodes_str = config.get<std::string>("", "REDIS_SENTINEL_NODES", "");
                std::vector<std::pair<std::string, int>> sentinels;

                if (!sentinel_nodes_str.empty()) {
                    sentinels = Cache::parse_sentinel_nodes_csv(sentinel_nodes_str);
                } else {
                    try {
                        const auto& cfg = config.get_json();
                        if (cfg.contains("cache") && cfg["cache"].contains("sentinel") &&
                            cfg["cache"]["sentinel"].contains("nodes")) {
                            const auto& nodes = cfg["cache"]["sentinel"]["nodes"];
                            if (nodes.is_array()) {
                                for (const auto& node : nodes) {
                                    sentinels.emplace_back(node.at("host").get<std::string>(),
                                                           node.at("port").get<int>());
                                }
                            }
                        }
                    } catch (...) {}
                }

                Jobs::get().init_blocking_client_sentinel(master_name,
                                                          sentinels,
                                                          brpop_timeout,
                                                          redis_password,
                                                          sentinel_password,
                                                          /*pool_size=*/std::max(concurrency, 4));
            } else {
                std::string redis_url = config.get<std::string>("cache.url", "REDIS_URL", "tcp://127.0.0.1:6379");
                const Cache::RedisAddress addr = Cache::parse_redis_url(redis_url);
                Jobs::get().init_blocking_client(
                    addr.host, addr.port, brpop_timeout, redis_password, /*pool_size=*/std::max(concurrency, 4));
            }
        }

        std::cout << "Worker ID:    " << worker_id << std::endl;
        std::cout << "Queue types:  ";
        for (size_t i = 0; i < worker_types.size(); ++i) {
            if (i > 0)
                std::cout << ", ";
            std::cout << worker_types[i];
        }
        std::cout << std::endl;
        std::cout << "Concurrency:  " << concurrency << std::endl;
        std::cout << "Health port:  " << health_port << std::endl;
        std::cout << "BRPOP timeout:" << brpop_timeout << "s" << std::endl;

        // Register minimal health endpoints via lambda handlers
        drogon::app().registerHandler(
            "/healthz",
            [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                json body = {{"status", "alive"}, {"service", "worker"}};
                resp->setBody(body.dump());
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                callback(resp);
            },
            {drogon::Get});

        drogon::app().registerHandler(
            "/ready",
            [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                // While draining we want k8s to stop scheduling new work to this
                // pod. Jobs::health_check() may still pass (Redis is up) but we
                // want the Deployment to pull the pod out of rotation ASAP.
                if (Core::is_shutting_down()) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    json body = {{"status", "draining"}, {"service", "worker"}};
                    resp->setBody(body.dump());
                    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    resp->setStatusCode(drogon::k503ServiceUnavailable);
                    callback(resp);
                    return;
                }
                bool ready = Jobs::is_initialized() && Jobs::get().health_check();
                auto resp = drogon::HttpResponse::newHttpResponse();
                json body = {{"status", ready ? "ready" : "not_ready"}, {"service", "worker"}};
                resp->setBody(body.dump());
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setStatusCode(ready ? drogon::k200OK : drogon::k503ServiceUnavailable);
                callback(resp);
            },
            {drogon::Get});

        // Configure Drogon for health-only serving
        drogon::app().addListener("0.0.0.0", health_port).setThreadNum(1).setLogLevel(trantor::Logger::kWarn);

        // Start worker threads
        std::vector<std::thread> workers;
        for (int i = 0; i < concurrency; ++i) {
            std::string tid = worker_id + "-" + std::to_string(i);
            workers.emplace_back(worker_loop, tid, worker_types, brpop_timeout);
        }

        std::cout << "Worker ready — " << concurrency << " thread(s) processing " << worker_types.size() << " queue(s)"
                  << std::endl;
        std::cout << "Health endpoint: http://0.0.0.0:" << health_port << "/healthz" << std::endl;

        // Shutdown monitor: the signal handler only sets the flag (it can't
        // safely touch the event loop); this thread does the actual drain.
        std::thread shutdown_monitor([] {
            while (!shutdown_requested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            Core::begin_shutdown();
            spdlog::info("Shutdown requested — stopping Drogon event loop");
            drogon::app().quit();
        });

        // Safety net: if anything below throws, the try scope unwinds and these
        // joinable threads' destructors would std::terminate. Join them first
        // (and let the monitor exit by setting the flag + quitting Drogon). On
        // the normal path everything is already joined, so this is a no-op.
        struct ThreadJoiner {
            std::vector<std::thread>& workers;
            std::thread& monitor;
            ~ThreadJoiner() {
                shutdown_requested.store(true);
                drogon::app().quit();
                if (monitor.joinable())
                    monitor.join();
                for (auto& t : workers)
                    if (t.joinable())
                        t.join();
            }
        } thread_joiner{workers, shutdown_monitor};

        // Block on Drogon event loop (handles /healthz + /ready)
        drogon::app().run();

        // After Drogon stops, signal threads and join. Workers will exit
        // their BRPOP loop on the next timeout (brpop_timeout seconds) or
        // after finishing their current job, whichever comes first.
        shutdown_requested.store(true);
        Core::begin_shutdown();
        if (shutdown_monitor.joinable())
            shutdown_monitor.join();
        std::cout << "Waiting for worker threads to drain (up to " << (brpop_timeout + 1) << "s per worker)..."
                  << std::endl;

        for (auto& t : workers) {
            if (t.joinable())
                t.join();
        }

        Core::shutdown();
        std::cout << "Worker exited successfully" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        try {
            Core::shutdown();
        } catch (...) {}
        return 1;
    }
}
