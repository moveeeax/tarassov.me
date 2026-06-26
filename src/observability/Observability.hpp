/**
 * @file Observability.hpp
 * @brief Observability module for logging, metrics, and tracing
 * @details Integrates spdlog for logging, prometheus-cpp for metrics,
 *          and opentelemetry-cpp for distributed tracing
 */

#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <opentelemetry/exporters/ostream/span_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "observability/Trace.hpp"
#include "utils/Config.hpp"

namespace Observability {

namespace detail {

// Custom spdlog flag `%*` — emits the log message JSON-escaped so that
// embedded quotes, backslashes, and control characters can't break out
// of the `"msg":"..."` field. spdlog has no built-in JSON message flag,
// so we register this when `format=json`.
class JsonEscapedMessageFlag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
        for (char c : std::string_view(msg.payload.data(), msg.payload.size())) {
            switch (c) {
                case '"':
                    dest.push_back('\\');
                    dest.push_back('"');
                    break;
                case '\\':
                    dest.push_back('\\');
                    dest.push_back('\\');
                    break;
                case '\n':
                    dest.push_back('\\');
                    dest.push_back('n');
                    break;
                case '\r':
                    dest.push_back('\\');
                    dest.push_back('r');
                    break;
                case '\t':
                    dest.push_back('\\');
                    dest.push_back('t');
                    break;
                case '\b':
                    dest.push_back('\\');
                    dest.push_back('b');
                    break;
                case '\f':
                    dest.push_back('\\');
                    dest.push_back('f');
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        for (char* p = buf; *p != '\0'; ++p)
                            dest.push_back(*p);
                    } else {
                        dest.push_back(c);
                    }
            }
        }
    }
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return spdlog::details::make_unique<JsonEscapedMessageFlag>();
    }
};

// Custom spdlog flag `%@` — emits the current request's trace id (the 2nd field
// of the thread-local W3C traceparent), or nothing when there's no active trace.
// Lets EVERY log line (not just the access-log line) be correlated to a trace in
// Jaeger/Tempo, which the RUNBOOK/README promise.
class TraceIdFlag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& dest) override {
        const std::string tp = Trace::current_traceparent();  // "00-<trace_id>-<span_id>-<flags>"
        const auto first = tp.find('-');
        if (first == std::string::npos)
            return;
        const auto second = tp.find('-', first + 1);
        if (second == std::string::npos)
            return;
        const std::string_view tid(tp.data() + first + 1, second - first - 1);
        dest.append(tid.data(), tid.data() + tid.size());
    }
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return spdlog::details::make_unique<TraceIdFlag>();
    }
};

}  // namespace detail

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;
namespace prometheus_ns = prometheus;

/**
 * @brief Logging subsystem
 */
class Logger {
private:
    std::shared_ptr<spdlog::logger> logger;

public:
    /**
     * @brief Initialize logger with console and file sinks
     * @param name Logger name
     * @param log_file Path to log file (optional)
     */
    // format: "text" (human-readable, default) or "json" (one JSON object
    // per line — feed-ready for Loki / ELK / Datadog). We hand-roll the
    // JSON pattern so spdlog still emits a single allocation per record.
    void initialize(const std::string& name = "app",
                    const std::string& log_file = "",
                    const std::string& format = "text",
                    const std::string& service_name = "") {
        std::vector<spdlog::sink_ptr> sinks;

        // Console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);
        sinks.push_back(console_sink);

        // File sink
        if (!log_file.empty()) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, 1024 * 1024 * 10, 3  // 10MB max, 3 files
            );
            file_sink->set_level(spdlog::level::info);
            sinks.push_back(file_sink);
        }

        logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::info);

        // spdlog's format tokens: %l=level, %n=logger, %v=message, %Y-... ISO8601.
        // JSON output uses our custom %* flag (registered via pattern_formatter)
        // to JSON-escape the message so quotes/newlines in payloads can't break
        // out of the "msg" field. We also pin service name here so aggregators
        // can filter without relying on container labels.
        if (format == "json") {
            std::string pat = R"({"ts":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l","logger":"%n")";
            if (!service_name.empty()) {
                pat += R"(,"service":")" + service_name + R"(")";
            }
            // trace_id is empty when there's no active trace — still a valid field.
            pat += R"(,"thread":%t,"trace_id":"%@","msg":"%*"})";
            auto formatter = std::make_unique<spdlog::pattern_formatter>();
            formatter->add_flag<detail::JsonEscapedMessageFlag>('*').add_flag<detail::TraceIdFlag>('@').set_pattern(
                pat);
            logger->set_formatter(std::move(formatter));
        } else {
            // Text mode: spdlog's default pattern plus a [trace=<id>] tag (empty
            // when no active trace) so human logs are correlatable too.
            auto formatter = std::make_unique<spdlog::pattern_formatter>();
            formatter->add_flag<detail::TraceIdFlag>('@').set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [trace=%@] %v");
            logger->set_formatter(std::move(formatter));
        }

        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
    }

    /**
     * @brief Set log level
     * @param level Level string (trace, debug, info, warn, error, critical)
     */
    void set_level(const std::string& level) {
        if (level == "trace")
            logger->set_level(spdlog::level::trace);
        else if (level == "debug")
            logger->set_level(spdlog::level::debug);
        else if (level == "info")
            logger->set_level(spdlog::level::info);
        else if (level == "warn")
            logger->set_level(spdlog::level::warn);
        else if (level == "error")
            logger->set_level(spdlog::level::err);
        else if (level == "critical")
            logger->set_level(spdlog::level::critical);
    }

    /**
     * @brief Get underlying spdlog logger
     * @return Shared pointer to logger
     */
    std::shared_ptr<spdlog::logger> get() { return logger; }

    /**
     * @brief Flush all logs
     */
    void flush() {
        if (logger) {
            logger->flush();
        }
    }

    /**
     * @brief Shutdown logger
     */
    void shutdown() {
        if (logger) {
            logger->flush();
            spdlog::shutdown();
        }
    }
};

/**
 * @brief Metrics subsystem using Prometheus
 */
class Metrics {
private:
    std::shared_ptr<prometheus_ns::Registry> registry;
    std::unique_ptr<prometheus_ns::Exposer> exposer;

public:
    /**
     * @brief Initialize metrics with Prometheus exposer.
     *        If @p bind_address is empty, the registry is created but no HTTP
     *        exposer is started — useful for CLI commands (--create-admin etc.)
     *        running alongside a live server in the same container.
     */
    void initialize(const std::string& bind_address = "0.0.0.0:9090") {
        registry = std::make_shared<prometheus_ns::Registry>();
        if (bind_address.empty())
            return;
        exposer = std::make_unique<prometheus_ns::Exposer>(bind_address);
        exposer->RegisterCollectable(registry);
    }

    /**
     * @brief Create a counter metric
     */
    prometheus_ns::Family<prometheus_ns::Counter>& create_counter(
        const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels = {}) {
        return prometheus_ns::BuildCounter().Name(name).Help(help).Labels(labels).Register(*registry);
    }

    /**
     * @brief Create a gauge metric
     */
    prometheus_ns::Family<prometheus_ns::Gauge>& create_gauge(const std::string& name,
                                                              const std::string& help,
                                                              const std::map<std::string, std::string>& labels = {}) {
        return prometheus_ns::BuildGauge().Name(name).Help(help).Labels(labels).Register(*registry);
    }

    /**
     * @brief Create a histogram family.
     * @note Bucket boundaries are set per-instance via Family::Add(labels, buckets),
     *       not on the family itself — the prometheus-cpp API does not carry
     *       default buckets at the family level. Use add_histogram() for a
     *       one-shot create-family-and-instance call.
     */
    prometheus_ns::Family<prometheus_ns::Histogram>& create_histogram(
        const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels = {}) {
        return prometheus_ns::BuildHistogram().Name(name).Help(help).Labels(labels).Register(*registry);
    }

    /**
     * @brief Create a histogram family and register a single instance with the given buckets.
     * @return Reference to the registered histogram ready for Observe() calls.
     */
    prometheus_ns::Histogram& add_histogram(const std::string& name,
                                            const std::string& help,
                                            const std::vector<double>& buckets,
                                            const std::map<std::string, std::string>& instance_labels = {},
                                            const std::map<std::string, std::string>& family_labels = {}) {
        auto& family = create_histogram(name, help, family_labels);
        return family.Add(instance_labels, buckets);
    }

    /**
     * @brief Get registry
     */
    std::shared_ptr<prometheus_ns::Registry> get_registry() { return registry; }

    /**
     * @brief Shutdown metrics
     */
    void shutdown() {
        exposer.reset();
        registry.reset();
    }
};

/**
 * @brief Tracing subsystem using OpenTelemetry
 */
class Tracer {
private:
    std::shared_ptr<trace_api::TracerProvider> provider;

public:
    /**
     * @brief Initialize tracing
     * @param service_name Service name for traces
     * @param otlp_endpoint OTLP HTTP endpoint (empty = OStream fallback)
     */
    void initialize(const std::string& service_name = "cpp_api_service", const std::string& otlp_endpoint = "") {
        // No endpoint → leave the global no-op TracerProvider in place. The
        // old OStream fallback exported EVERY span synchronously to stdout
        // under a global mutex (SimpleSpanProcessor) — a per-request hot-path
        // cost on the default config. AutoSpan in Database.hpp already relies
        // on the no-op provider being cheap. Set OTLP_ENDPOINT to enable real
        // tracing (or observability.trace_stdout=true for the debug dump).
        bool stdout_trace = false;
        if (otlp_endpoint.empty() && Config::is_initialized())
            stdout_trace = Config::get().get<bool>("observability.trace_stdout", "TRACE_STDOUT", false);
        if (otlp_endpoint.empty() && !stdout_trace) {
            spdlog::info("Tracing: no OTLP endpoint and trace_stdout=false — using no-op tracer");
            return;
        }

        auto resource_attrs = resource::ResourceAttributes{{"service.name", service_name}};
        auto resource = resource::Resource::Create(resource_attrs);

        std::unique_ptr<trace_sdk::SpanProcessor> processor;
        if (!otlp_endpoint.empty()) {
            otlp::OtlpHttpExporterOptions opts;
            opts.url = otlp_endpoint;
            auto exporter = otlp::OtlpHttpExporterFactory::Create(opts);

            trace_sdk::BatchSpanProcessorOptions batch_opts;
            processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(exporter), batch_opts);
        } else {
            auto exporter = std::make_unique<opentelemetry::exporter::trace::OStreamSpanExporter>();
            processor = std::make_unique<trace_sdk::SimpleSpanProcessor>(std::move(exporter));
        }

        provider = trace_sdk::TracerProviderFactory::Create(std::move(processor), resource);
        trace_api::Provider::SetTracerProvider(provider);
    }

    /**
     * @brief Get a tracer instance
     */
    opentelemetry::nostd::shared_ptr<trace_api::Tracer> get_tracer(const std::string& name = "default") {
        return trace_api::Provider::GetTracerProvider()->GetTracer(name);
    }

    /**
     * @brief Shutdown tracing. Flush + shut down the SDK provider so buffered
     *        spans are exported, then point the GLOBAL provider back at the
     *        no-op — otherwise Database::AutoSpan (which reads the global
     *        provider, not this member) would keep minting spans into a
     *        half-torn-down pipeline during the rest of Core::shutdown.
     */
    void shutdown() {
        // Take ownership up front so a concurrent second shutdown() (e.g. a
        // double SIGTERM) sees provider==null and bails — OTel's SDK
        // Shutdown() isn't guaranteed idempotent.
        auto p = std::move(provider);
        provider = nullptr;
        if (!p)
            return;
        if (auto* sdk = dynamic_cast<trace_sdk::TracerProvider*>(p.get())) {
            sdk->ForceFlush(std::chrono::microseconds(2'000'000));
            sdk->Shutdown(std::chrono::microseconds(2'000'000));
        }
        trace_api::Provider::SetTracerProvider(
            opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>(new trace_api::NoopTracerProvider()));
    }
};

/**
 * @brief Main observability manager
 */
class ObservabilitySystem {
private:
    Logger logger_system;
    Metrics metrics_system;
    Tracer tracer_system;
    bool initialized = false;

public:
    /**
     * @brief Initialize all observability subsystems
     */
    void initialize(const std::string& log_name = "app",
                    const std::string& log_file = "logs/app.log",
                    const std::string& metrics_addr = "0.0.0.0:9090",
                    const std::string& service_name = "cpp_api_service",
                    const std::string& otlp_endpoint = "",
                    const std::string& log_format = "text") {
        if (initialized) {
            throw std::runtime_error("Observability system already initialized");
        }

        logger_system.initialize(log_name, log_file, log_format, service_name);
        metrics_system.initialize(metrics_addr);
        tracer_system.initialize(service_name, otlp_endpoint);

        initialized = true;

        spdlog::info("Observability system initialized");
        if (!metrics_addr.empty())
            spdlog::info("Metrics available at: http://{}/metrics", metrics_addr);
        else
            spdlog::info("Metrics exposer disabled (empty bind address)");
    }

    Logger& logger() { return logger_system; }
    Metrics& metrics() { return metrics_system; }
    Tracer& tracer() { return tracer_system; }

    void shutdown() {
        if (initialized) {
            spdlog::info("Shutting down observability system");
            tracer_system.shutdown();
            metrics_system.shutdown();
            logger_system.shutdown();
            initialized = false;
        }
    }

    bool is_initialized() const { return initialized; }
};

/**
 * @brief Global observability instance
 */
inline std::unique_ptr<ObservabilitySystem> global_observability = nullptr;

inline void initialize(const std::string& log_name = "app",
                       const std::string& log_file = "logs/app.log",
                       const std::string& metrics_addr = "0.0.0.0:9090",
                       const std::string& service_name = "cpp_api_service",
                       const std::string& otlp_endpoint = "",
                       const std::string& log_format = "text") {
    if (global_observability != nullptr) {
        throw std::runtime_error("Observability already initialized");
    }
    global_observability = std::make_unique<ObservabilitySystem>();
    global_observability->initialize(log_name, log_file, metrics_addr, service_name, otlp_endpoint, log_format);
}

inline ObservabilitySystem& get() {
    if (global_observability == nullptr) {
        throw std::runtime_error("Observability not initialized");
    }
    return *global_observability;
}

inline bool is_initialized() {
    return global_observability != nullptr && global_observability->is_initialized();
}

inline void shutdown() {
    if (global_observability) {
        global_observability->shutdown();
        global_observability.reset();
    }
}

}  // namespace Observability
