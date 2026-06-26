/**
 * @file Job.hpp
 * @brief Job data model + Redis key schema for the job queue.
 * @details Split out of Jobs.hpp so consumers that only need the model
 *          (tests, the worker dispatcher) don't pull the queue manager.
 */

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <uuid/uuid.h>

#include <nlohmann/json.hpp>

#include "utils/Time.hpp"

namespace Jobs {

using json = nlohmann::json;

/**
 * @brief Thrown by a job handler when the failure is PERMANENT (e.g. no handler
 *        registered for the type) — retrying would only burn cycles. The worker
 *        routes these straight to the DLQ instead of through the retry budget.
 */
struct PermanentJobError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Generate a UUID v4 string via libuuid (uuid_generate_random).
 */
inline std::string generate_uuid() {
    uuid_t bin;
    uuid_generate_random(bin);
    char buf[37];  // 36 chars + NUL
    uuid_unparse_lower(bin, buf);
    return std::string(buf, 36);
}

/**
 * @brief Job data structure
 */
struct Job {
    std::string id;
    std::string type;
    json payload;
    std::string status;  // pending, processing, completed, failed
    json result;
    std::string error;
    std::string worker_id;
    std::string trace_id;  // hex trace id of the worker's processing span (empty until picked)
    // W3C traceparent of the ORIGINATING request (empty when enqueued outside a
    // request, e.g. scheduled tasks). Lets the worker continue the same
    // distributed trace instead of starting an unrelated root span.
    std::string traceparent;
    int retry_count = 0;
    int max_retries = 3;
    int64_t created_at = 0;
    int64_t updated_at = 0;

    json to_json() const {
        return {{"id", id},
                {"type", type},
                {"payload", payload},
                {"status", status},
                {"result", result},
                {"error", error},
                {"worker_id", worker_id},
                {"trace_id", trace_id},
                {"traceparent", traceparent},
                {"retry_count", retry_count},
                {"max_retries", max_retries},
                {"created_at", created_at},
                {"updated_at", updated_at}};
    }

    static Job from_json(const json& j) {
        Job job;
        job.id = j.at("id").get<std::string>();
        job.type = j.at("type").get<std::string>();
        job.payload = j.value("payload", json::object());
        job.status = j.at("status").get<std::string>();
        job.result = j.value("result", json(nullptr));
        job.error = j.value("error", "");
        job.worker_id = j.value("worker_id", "");
        job.trace_id = j.value("trace_id", "");
        job.traceparent = j.value("traceparent", "");
        job.retry_count = j.value("retry_count", 0);
        job.max_retries = j.value("max_retries", 3);
        job.created_at = j.value("created_at", int64_t(0));
        job.updated_at = j.value("updated_at", int64_t(0));
        return job;
    }
};

/**
 * @brief Redis key helpers
 */
inline std::string job_key(const std::string& id) {
    return "job:" + id;
}
inline std::string queue_key(const std::string& type) {
    return "jobs:queue:" + type;
}
inline std::string dlq_key(const std::string& type) {
    return "jobs:dlq:" + type;
}
/**
 * @brief Time-ordered indexes backing paginated listing (sorted sets,
 *        score = created_at): one global, one per type. Entries can
 *        outlive the job blob (which expires via result_ttl) — list_paged()
 *        heals such stale ids lazily as pages touch them.
 */
inline std::string index_key() {
    return "jobs:index";
}
inline std::string index_key_for(const std::string& type) {
    return "jobs:index:" + type;
}
// Per-worker in-flight list. After a successful BLMOVE the id sits here
// until complete() / fail() removes it. On crash recovery (the worker
// restarts with the same id) we scan this list and push its contents back
// onto the live queue, so no job is lost to a mid-processing crash.
inline std::string processing_key(const std::string& worker_id) {
    return "jobs:processing:" + worker_id;
}
// Backoff retry set (sorted set, score = ready-at epoch millis): a failed job
// with backoff enabled parks here until promote_due_jobs() moves it back onto
// its live queue. Global, not per-type — the job blob carries the type.
inline std::string delayed_key() {
    return "jobs:delayed";
}
// Visibility-timeout leases (sorted set, score = expires-at epoch millis): a
// picked job is leased here when visibility timeout is enabled; the reaper
// reclaims any lease whose deadline has passed (a worker that died mid-job).
inline std::string leases_key() {
    return "jobs:leases";
}
inline constexpr const char* kDlqAllKey = "jobs:dlq:_all";

/**
 * @brief Current epoch timestamp in seconds
 */
inline int64_t now_epoch() {
    return Utils::Time::now_epoch_seconds();
}

}  // namespace Jobs
