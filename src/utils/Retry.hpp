/**
 * @file Retry.hpp
 * @brief Generic retry-with-backoff helper + transient-error classifiers.
 * @details The callable is re-invoked up to max_attempts times when the
 *          classifier marks the raised exception as transient. Backoff is
 *          exponential with optional full-jitter. Intended for wrapping
 *          database, cache and messaging calls that may hit momentary
 *          network or connection-lifecycle errors.
 *
 * Usage:
 * @code
 *   auto result = Retry::run(
 *       [&]{ return do_stuff(); },
 *       Retry::is_transient_pqxx,
 *       Retry::Policy{},
 *       "db");
 * @endcode
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <pqxx/pqxx>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

namespace Retry {

struct Policy {
    int max_attempts = 3;
    int base_delay_ms = 100;
    int max_delay_ms = 2000;
    bool jitter = true;
};

// Pluggable counter sink. A non-null `on_event` is invoked for every
// retry and every exhaustion so the Observability module can bump a
// Prometheus counter without Retry having to depend on Prometheus headers.
// Set via bind_metrics(); the function pointer is plain data and does
// not involve any global constructor that could misbehave at load time.
using MetricFn = void (*)(const char* component, const char* outcome);
// Bound on the main thread (Core::init), read from every IO/worker thread —
// atomic so the publish is visible without a data race.
inline std::atomic<MetricFn> metric_sink_{nullptr};

inline void bind_metrics(MetricFn fn) {
    metric_sink_.store(fn, std::memory_order_relaxed);
}

// Unbind the sink. Call this when the backing Prometheus family is about
// to be destroyed (e.g. Observability::shutdown) — otherwise subsequent
// retry events try to dereference a freed family and segfault.
inline void reset_metrics() {
    metric_sink_.store(nullptr, std::memory_order_relaxed);
}

inline void inc_metric_(const char* component, const char* outcome) {
    if (auto fn = metric_sink_.load(std::memory_order_relaxed)) {
        try {
            fn(component, outcome);
        } catch (...) {}
    }
}

namespace detail {

inline int compute_delay_ms(int attempt_zero_based, const Policy& p) {
    // Exponential: base * 2^attempt, capped at max.
    // attempt_zero_based is the index of the FAILED attempt (0, 1, 2, ...)
    const int shift = std::min(attempt_zero_based, 10);
    const int64_t expo = static_cast<int64_t>(p.base_delay_ms) << shift;
    int delay = static_cast<int>(std::min<int64_t>(p.max_delay_ms, expo));
    if (p.jitter) {
        // Full jitter: uniform [0, delay].
        thread_local std::mt19937 rng(
            static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int> dist(0, std::max(1, delay));
        delay = dist(rng);
    }
    return delay;
}

}  // namespace detail

/**
 * @brief Call @p fn, retrying transient failures up to @p p.max_attempts.
 * @tparam Fn    Callable returning any type (including void).
 * @tparam Pred  Callable bool(const std::exception&) classifying transience.
 * @param component Short label used for logs ("db", "redis", "kafka").
 * @throws The last exception if @p p.max_attempts is exhausted or the
 *         exception is classified as non-transient.
 */
template <typename Fn, typename Pred>
auto run(Fn&& fn, Pred&& is_transient, const Policy& p, const std::string& component) -> std::invoke_result_t<Fn> {
    int attempt = 0;
    for (;;) {
        try {
            return fn();
        } catch (const std::exception& e) {
            ++attempt;
            const bool can_retry = attempt < p.max_attempts && is_transient(e);
            if (!can_retry) {
                if (attempt >= p.max_attempts) {
                    spdlog::warn("[retry:{}] giving up after {} attempts: {}", component, attempt, e.what());
                    inc_metric_(component.c_str(), "exhausted");
                }
                throw;
            }
            const int delay_ms = detail::compute_delay_ms(attempt - 1, p);
            spdlog::warn("[retry:{}] transient error (attempt {}/{}): {} — retrying in {}ms",
                         component,
                         attempt,
                         p.max_attempts,
                         e.what(),
                         delay_ms);
            inc_metric_(component.c_str(), "retried");
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
}

// ---------------------------------------------------------------------------
// Classifiers
// ---------------------------------------------------------------------------

/**
 * @brief Classifier for READ transactions. Safe to retry liberally because
 *        a duplicate SELECT has no side effects on the caller.
 *        Retries on:
 *          - broken_connection
 *          - class 08 (connection exception)
 *          - 57P0x (operator intervention: shutdown / crash / can't connect)
 *          - 40001 (serialization_failure) / 40P01 (deadlock_detected)
 */
inline bool is_transient_pqxx_read(const std::exception& e) {
    if (dynamic_cast<const pqxx::broken_connection*>(&e) != nullptr)
        return true;
    if (const auto* sqle = dynamic_cast<const pqxx::sql_error*>(&e)) {
        std::string ss(sqle->sqlstate());
        if (ss.size() >= 2 && ss[0] == '0' && ss[1] == '8')
            return true;  // Class 08
        if (ss == "40001" || ss == "40P01")
            return true;
        if (ss.size() >= 3 && ss[0] == '5' && ss[1] == '7' && ss[2] == 'P')
            return true;
    }
    return false;
}

/**
 * @brief Classifier for WRITE transactions. Deliberately CONSERVATIVE:
 *        only retries errors where PG is guaranteed to have rolled the
 *        transaction back with zero side effects.
 *
 *        Retried:
 *          - 40001 (serialization_failure): txn aborted, replay is safe.
 *          - 40P01 (deadlock_detected): txn aborted, replay is safe.
 *
 *        NOT retried here (could cause double-insert):
 *          - broken_connection / class 08: the commit may or may not have
 *            made it to the server before the socket died. A retry of a
 *            non-idempotent INSERT can duplicate the row.
 *
 *        Callers that know their write is idempotent (e.g. UPSERT, or a
 *        call wrapped by the Idempotency-Key middleware) can pass the
 *        read classifier explicitly to opt into aggressive retry.
 */
inline bool is_transient_pqxx_write(const std::exception& e) {
    if (const auto* sqle = dynamic_cast<const pqxx::sql_error*>(&e)) {
        std::string ss(sqle->sqlstate());
        if (ss == "40001" || ss == "40P01")
            return true;
    }
    return false;
}

/**
 * @brief Legacy alias — callers that don't distinguish read/write still
 *        work, but get the liberal read-style behaviour. Prefer the
 *        explicit _read / _write helpers in new code.
 */
inline bool is_transient_pqxx(const std::exception& e) {
    return is_transient_pqxx_read(e);
}

/**
 * @brief True if an exception from redis-plus-plus is transient.
 */
inline bool is_transient_redis(const std::exception& e) {
    using namespace sw::redis;
    if (dynamic_cast<const TimeoutError*>(&e) != nullptr)
        return true;
    if (dynamic_cast<const IoError*>(&e) != nullptr)
        return true;
    if (dynamic_cast<const ClosedError*>(&e) != nullptr)
        return true;
    // Other sw::redis::Error subclasses (ReplyError, WrongTypeError, ...) are
    // logical errors — not worth retrying.
    return false;
}

}  // namespace Retry
