/**
 * @file Jobs.hpp
 * @brief Redis-backed job queue module
 * @details Provides job submission, retrieval, and lifecycle management
 *          using Redis lists (LPUSH/BRPOP) for distributed work queues
 */

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "jobs/Job.hpp"
#include "observability/Trace.hpp"

namespace Jobs {

using json = nlohmann::json;

/**
 * @brief Job queue manager backed by Redis
 *
 * Exception contract (for callers adding new endpoints):
 *   - submit / pick / complete / fail / cancel / get_status / list_paged /
 *     list_dlq / requeue_from_dlq THROW on Redis errors or missing state —
 *     controllers map them to 5xx.
 *   - dlq_depth / dlq_depth_by_type / health_check / set_trace_id and the
 *     index ZADDs inside submit are best-effort: they swallow Redis errors
 *     (observability must not take the queue down with it).
 */
class JobQueue {
private:
    bool initialized_ = false;
    long result_ttl_ = 86400;  // seconds
    int default_max_retries_ = 3;
    // Retry backoff (opt-in). 0 = legacy behaviour: fail() requeues immediately.
    // >0 = a failed job is parked in jobs:delayed for an exponentially growing
    // delay (base * 2^(retry_count-1), capped at max), promoted back by
    // promote_due_jobs() — kills the tight pick→fail→pick loop on a near-empty
    // queue.
    int64_t retry_backoff_base_ms_ = 0;
    int64_t retry_backoff_max_ms_ = 60000;
    // Visibility timeout (opt-in). 0 = leases disabled (only same-worker startup
    // recovery). >0 = pick() leases the job in jobs:leases for this many ms;
    // reap_expired_leases() reclaims any lease a crashed worker never cleared,
    // re-running it via fail() (so a permanently-stuck job eventually DLQs).
    int64_t visibility_timeout_ms_ = 0;
    // shared_ptr (not unique_ptr) so pick() can hold a local copy for the
    // duration of its BRPOP: shutdown() dropping the manager's reference then
    // can't free the client out from under an in-flight blocking pop.
    std::shared_ptr<sw::redis::Redis> blocking_client_;  // for BRPOP (long socket_timeout)

public:
    void initialize(long result_ttl = 86400, int max_retries = 3) {
        if (initialized_) {
            throw std::runtime_error("JobQueue already initialized");
        }
        if (!Cache::is_initialized()) {
            throw std::runtime_error("Cache must be initialized before Jobs");
        }
        result_ttl_ = result_ttl;
        default_max_retries_ = max_retries;
        initialized_ = true;
        spdlog::info("Job queue initialized (result_ttl={}s, max_retries={})", result_ttl_, default_max_retries_);
    }

    /**
     * @brief Enable exponential retry backoff. base_ms=0 disables it (legacy
     *        immediate requeue). Called once during init from config.
     */
    void set_retry_backoff(int64_t base_ms, int64_t max_ms) {
        retry_backoff_base_ms_ = base_ms < 0 ? 0 : base_ms;
        retry_backoff_max_ms_ = max_ms < base_ms ? base_ms : max_ms;
    }

    /**
     * @brief Enable the visibility-timeout lease. sec<=0 disables it. Called
     *        once during init from config.
     */
    void set_visibility_timeout(int64_t sec) { visibility_timeout_ms_ = sec <= 0 ? 0 : sec * 1000; }

    /**
     * @brief Dedicated standalone Redis client for blocking BRPOP. The Cache
     *        pool has too tight a socket_timeout for blocking ops; this one
     *        uses (brpop_timeout + 2s) so the socket doesn't trip first.
     */
    void init_blocking_client(const std::string& host,
                              int port,
                              long brpop_timeout_sec,
                              const std::string& password = "",
                              int pool_size = 4) {
        const auto sock_to = std::chrono::milliseconds((brpop_timeout_sec + 2) * 1000);
        // One blocking connection is held per concurrent worker thread for the
        // duration of its BRPOP — size the pool to concurrency or threads beyond
        // the 4th stall waiting for a connection (silent throughput cliff).
        blocking_client_ = Cache::make_standalone_client(host, port, pool_size, password, sock_to, sock_to);
        blocking_client_->ping();
        spdlog::info(
            "Jobs blocking Redis client initialized ({}:{}, socket_timeout={}s)", host, port, brpop_timeout_sec + 2);
    }

    /**
     * @brief Dedicated Sentinel-aware Redis client for blocking BRPOP.
     */
    void init_blocking_client_sentinel(const std::string& master_name,
                                       const std::vector<std::pair<std::string, int>>& sentinels,
                                       long brpop_timeout_sec,
                                       const std::string& password = "",
                                       const std::string& sentinel_password = "",
                                       int pool_size = 4) {
        const auto sock_to = std::chrono::milliseconds((brpop_timeout_sec + 2) * 1000);
        // Size to worker concurrency — see init_blocking_client.
        blocking_client_ = Cache::make_sentinel_client(
            master_name, sentinels, pool_size, password, sentinel_password, sock_to, sock_to);
        blocking_client_->ping();
        spdlog::info("Jobs blocking Redis client initialized via Sentinel (master: {}, socket_timeout={}s)",
                     master_name,
                     brpop_timeout_sec + 2);
    }

    /**
     * @brief Submit a new job to the queue
     */
    Job submit(const std::string& type, const json& payload, int max_retries = -1) {
        check_initialized();
        auto& redis = Cache::get().get_client();

        Job job;
        job.id = generate_uuid();
        job.type = type;
        job.payload = payload;
        job.status = "pending";
        job.result = nullptr;
        job.retry_count = 0;
        job.max_retries = (max_retries >= 0) ? max_retries : default_max_retries_;
        job.created_at = now_epoch();
        job.updated_at = job.created_at;
        // Carry the originating request's trace context (if we're inside a
        // request) so the worker can continue the same distributed trace.
        job.traceparent = Observability::Trace::current_traceparent();

        // Store job data
        redis.set(job_key(job.id), job.to_json().dump());

        // Push job ID to the type-specific queue
        redis.lpush(queue_key(type), job.id);

        // Time-ordered indexes for paginated listing. Best-effort: a missed
        // ZADD only means the job won't show in the admin list.
        try {
            const double score = static_cast<double>(job.created_at);
            redis.zadd(index_key(), job.id, score);
            redis.zadd(index_key_for(type), job.id, score);
        } catch (const std::exception& e) {
            spdlog::warn("Jobs: failed to index job {}: {}", job.id, e.what());
        }

        spdlog::debug("Job submitted: id={} type={}", job.id, type);
        return job;
    }

    /**
     * @brief Pick the next job from one or more queues (blocking pop).
     * @details Uses BRPOPLPUSH (or multi-key BRPOP + RPUSH fallback) to move
     *          the id atomically from the live queue into the worker's
     *          per-worker processing list, so a worker crashing between
     *          "got id" and "set status=processing" doesn't lose the job:
     *          recover_processing() on startup pushes leftovers back.
     *          Only single-type picks get true atomicity; multi-type picks
     *          fall back to BRPOP + explicit RPUSH to processing list
     *          (a narrow window remains but shrinks from "full processing"
     *          to a single command — acceptable trade-off).
     * @param types Queue types to pop from
     * @param timeout_sec BRPOP timeout (0 = block forever)
     * @param worker_id Worker claiming this job
     * @return Job if one was available, nullopt on timeout
     */
    std::optional<Job> pick(const std::vector<std::string>& types,
                            long timeout_sec,
                            const std::string& worker_id = "") {
        check_initialized();
        // Local copy keeps the blocking client alive across the BRPOP even if
        // shutdown() resets the member concurrently (see member declaration).
        auto blocking = blocking_client_;
        auto& brpop_redis = blocking ? *blocking : Cache::get().get_client();
        auto& redis = Cache::get().get_client();

        const std::string proc_list = processing_key(worker_id);
        std::string job_id;

        if (types.size() == 1) {
            // BRPOPLPUSH is atomic: id moves from queue → processing list in
            // one round-trip. redis-plus-plus exposes this as brpoplpush.
            auto moved = brpop_redis.brpoplpush(queue_key(types[0]), proc_list, std::chrono::seconds(timeout_sec));
            if (!moved)
                return std::nullopt;
            job_id = *moved;
        } else {
            // Multi-key blocking pop has no atomic move variant in classic
            // Redis (BLMPOP exists in 7.0+ but the client doesn't expose it
            // uniformly). Accept a small window: BRPOP → RPUSH to processing.
            std::vector<std::string> keys;
            keys.reserve(types.size());
            for (const auto& t : types)
                keys.push_back(queue_key(t));
            auto result = brpop_redis.brpop(keys.begin(), keys.end(), timeout_sec);
            if (!result)
                return std::nullopt;
            job_id = result->second;
            try {
                redis.rpush(proc_list, job_id);
            } catch (...) {
                // The id is already off the queue. If we can't record it in the
                // processing list it would be stranded (neither queued nor
                // tracked, so recovery can't rescue it) — put it back on its
                // queue (result->first) instead of silently losing the job.
                try {
                    redis.rpush(result->first, job_id);
                } catch (...) {}
                spdlog::warn("Job {} pick: failed to record in processing list — requeued", job_id);
                return std::nullopt;
            }
        }

        // Fetch job data
        auto data = redis.get(job_key(job_id));
        if (!data) {
            spdlog::warn("Job {} popped from queue but data not found in Redis", job_id);
            // Clean up — ghost id shouldn't linger in processing list.
            try {
                redis.lrem(proc_list, 0, job_id);
            } catch (...) {}
            return std::nullopt;
        }

        auto job = Job::from_json(json::parse(*data));
        job.status = "processing";
        job.worker_id = worker_id;
        job.updated_at = now_epoch();

        // Update status in Redis
        redis.set(job_key(job.id), job.to_json().dump());

        // Lease the job so a cross-worker reaper can reclaim it if this worker
        // dies before complete()/fail(). Best-effort: a missed lease only loses
        // the visibility-timeout safety net, not the job (startup recovery still
        // covers a same-id restart).
        if (visibility_timeout_ms_ > 0) {
            try {
                redis.zadd(leases_key(),
                           job.id,
                           static_cast<double>(Utils::Time::now_epoch_millis() + visibility_timeout_ms_));
            } catch (...) {}
        }

        spdlog::debug("Job picked: id={} type={} worker={}", job.id, job.type, worker_id);
        return job;
    }

    /**
     * @brief Mark a job as completed with result
     */
    void complete(const std::string& id, const json& result) {
        check_initialized();
        auto& redis = Cache::get().get_client();

        auto data = redis.get(job_key(id));
        if (!data) {
            throw std::runtime_error("Job not found: " + id);
        }

        auto job = Job::from_json(json::parse(*data));
        job.status = "completed";
        job.result = result;
        job.updated_at = now_epoch();

        redis.setex(job_key(id), result_ttl_, job.to_json().dump());
        // Drop from the worker's processing list — the job is officially done
        // and no recovery pass should resurrect it.
        if (!job.worker_id.empty()) {
            try {
                redis.lrem(processing_key(job.worker_id), 0, id);
            } catch (...) {}
        }
        clear_lease_(redis, id);
        spdlog::debug("Job completed: id={}", id);
    }

    /**
     * @brief Mark a job as failed; requeue if retries remain
     */
    void fail(const std::string& id, const std::string& error) {
        check_initialized();
        auto& redis = Cache::get().get_client();

        auto data = redis.get(job_key(id));
        if (!data) {
            throw std::runtime_error("Job not found: " + id);
        }

        auto job = Job::from_json(json::parse(*data));
        job.retry_count++;
        job.updated_at = now_epoch();

        // Remove from the worker's processing list whether we requeue or DLQ —
        // recovery should never double-process a job that already failed once.
        if (!job.worker_id.empty()) {
            try {
                redis.lrem(processing_key(job.worker_id), 0, id);
            } catch (...) {}
        }
        clear_lease_(redis, id);

        if (job.retry_count < job.max_retries) {
            // Requeue for retry.
            job.status = "pending";
            job.error = error;
            redis.set(job_key(id), job.to_json().dump());
            const int64_t delay_ms = backoff_delay_ms_(job.retry_count);
            if (delay_ms > 0) {
                // Park in the delayed set; promote_due_jobs() returns it to the
                // live queue once the backoff window elapses. Avoids a tight
                // pick→fail→pick loop when the queue is otherwise empty.
                redis.zadd(delayed_key(), id, static_cast<double>(Utils::Time::now_epoch_millis() + delay_ms));
                spdlog::info(
                    "Job {} retry {}/{} scheduled in {}ms: {}", id, job.retry_count, job.max_retries, delay_ms, error);
            } else {
                redis.lpush(queue_key(job.type), id);
                spdlog::info("Job {} requeued (retry {}/{}): {}", id, job.retry_count, job.max_retries, error);
            }
        } else {
            // Max retries exceeded — send to dead-letter queue for inspection.
            // DLQ entries are NOT reaped by result_ttl_ (operator must drain
            // or requeue them explicitly via /api/jobs/dlq/{id}/requeue).
            job.status = "dead";
            job.error = error;
            redis.set(job_key(id), job.to_json().dump());
            redis.lpush(dlq_key(job.type), id);
            redis.sadd(kDlqAllKey, id);  // index for "list all DLQ" queries
            spdlog::warn("Job {} DLQ'd after {} retries: {}", id, job.retry_count, error);
        }
    }

    /**
     * @brief Send a job STRAIGHT to the dead-letter queue, bypassing the retry
     *        budget — for permanent failures (PermanentJobError, e.g. no handler
     *        for the type) where retrying only burns tight pick→throw→requeue
     *        cycles. Mirrors fail()'s DLQ branch without the retry path.
     */
    void dead_letter(const std::string& id, const std::string& error) {
        check_initialized();
        auto& redis = Cache::get().get_client();
        auto data = redis.get(job_key(id));
        if (!data)
            throw std::runtime_error("Job not found: " + id);
        auto job = Job::from_json(json::parse(*data));
        job.updated_at = now_epoch();
        if (!job.worker_id.empty()) {
            try {
                redis.lrem(processing_key(job.worker_id), 0, id);
            } catch (...) {}
        }
        clear_lease_(redis, id);
        job.status = "dead";
        job.error = error;
        // Index BEFORE committing the blob: the 3 writes aren't atomic, and if
        // lpush/sadd threw after the blob was already marked "dead" the job
        // would vanish from list_dlq/requeue/recover entirely. Indexing first
        // means a failure leaves it discoverable (with its prior status) and
        // list_dlq heals once the blob write lands.
        redis.lpush(dlq_key(job.type), id);
        redis.sadd(kDlqAllKey, id);
        redis.set(job_key(id), job.to_json().dump());
        spdlog::warn("Job {} sent straight to DLQ (permanent failure): {}", id, error);
    }

    /**
     * @brief List jobs currently in the dead-letter queue.
     * @param type Filter by job type (empty = all DLQ).
     * @param limit Max results.
     */
    std::vector<Job> list_dlq(const std::string& type = "", int limit = 100) {
        check_initialized();
        auto& redis = Cache::get().get_client();
        std::vector<std::string> ids;
        if (type.empty()) {
            // Scan the index set.
            redis.smembers(kDlqAllKey, std::back_inserter(ids));
        } else {
            redis.lrange(dlq_key(type), 0, limit - 1, std::back_inserter(ids));
        }
        if (static_cast<int>(ids.size()) > limit)
            ids.resize(limit);
        // One MGET instead of a GET per id (avoids N round-trips).
        return fetch_jobs_by_ids_(ids);
    }

    /**
     * @brief Move a DLQ job back to its live queue, resetting retry_count.
     * @return true if the job was found and requeued; false otherwise.
     */
    bool requeue_from_dlq(const std::string& id) {
        check_initialized();
        auto& redis = Cache::get().get_client();
        auto data = redis.get(job_key(id));
        if (!data)
            return false;
        Job job;
        try {
            job = Job::from_json(json::parse(*data));
        } catch (...) {
            return false;
        }
        if (job.status != "dead")
            return false;

        // Remove from per-type DLQ list + index.
        redis.lrem(dlq_key(job.type), 0, id);
        redis.srem(kDlqAllKey, id);

        job.status = "pending";
        job.retry_count = 0;
        job.error.clear();
        job.updated_at = now_epoch();
        redis.set(job_key(id), job.to_json().dump());
        redis.lpush(queue_key(job.type), id);
        spdlog::info("Job {} requeued from DLQ", id);
        return true;
    }

    /**
     * @brief Current total DLQ depth across all types.
     */
    long dlq_depth() {
        if (!initialized_)
            return 0;
        try {
            return static_cast<long>(Cache::get().get_client().scard(kDlqAllKey));
        } catch (...) {
            return 0;
        }
    }

    /**
     * @brief Depth per job type. Scans each `jobs:dlq:*` list to return
     *        {type → depth}. Used by the metrics refresher so dashboards
     *        can see which specific job type is clogged.
     */
    std::unordered_map<std::string, long> dlq_depth_by_type() {
        std::unordered_map<std::string, long> out;
        if (!initialized_)
            return out;
        try {
            auto& redis = Cache::get().get_client();
            std::vector<std::string> keys;
            long long cursor = 0;
            do {
                std::vector<std::string> batch;
                cursor = redis.scan(cursor, "jobs:dlq:*", 100, std::back_inserter(batch));
                for (auto& k : batch) {
                    if (k == kDlqAllKey)
                        continue;  // skip the `_all` index set
                    long long len = redis.llen(k);
                    // strip "jobs:dlq:" prefix
                    const std::string prefix = "jobs:dlq:";
                    std::string type = k.rfind(prefix, 0) == 0 ? k.substr(prefix.size()) : k;
                    out[std::move(type)] = static_cast<long>(len);
                }
            } while (cursor != 0);
        } catch (const std::exception& e) {
            spdlog::warn("dlq_depth_by_type: scan failed: {}", e.what());
        }
        return out;
    }

    /**
     * @brief Live depth per job type for the WAITING queue (`jobs:queue:*`),
     *        as {type → depth}. The leading-indicator counterpart to
     *        dlq_depth_by_type (which is the lagging "already gave up" signal):
     *        a climbing queue depth means submitters are outrunning the
     *        worker pool before anything reaches the DLQ. Used by the metrics
     *        refresher so dashboards/alerts can see a backlog forming.
     */
    std::unordered_map<std::string, long> queue_depth_by_type() {
        std::unordered_map<std::string, long> out;
        if (!initialized_)
            return out;
        try {
            auto& redis = Cache::get().get_client();
            long long cursor = 0;
            do {
                std::vector<std::string> batch;
                cursor = redis.scan(cursor, "jobs:queue:*", 100, std::back_inserter(batch));
                for (auto& k : batch) {
                    long long len = redis.llen(k);
                    // strip "jobs:queue:" prefix
                    const std::string prefix = "jobs:queue:";
                    std::string type = k.rfind(prefix, 0) == 0 ? k.substr(prefix.size()) : k;
                    out[std::move(type)] = static_cast<long>(len);
                }
            } while (cursor != 0);
        } catch (const std::exception& e) {
            spdlog::warn("queue_depth_by_type: scan failed: {}", e.what());
        }
        return out;
    }

    /**
     * @brief Return any jobs left in this worker's processing list to the
     *        live queue. Call once at worker startup: if the previous
     *        instance with the same WORKER_ID crashed between BRPOPLPUSH
     *        and complete()/fail(), the id stranded in jobs:processing:*
     *        gets rescued and re-executed.
     * @return number of jobs rescued.
     */
    long recover_processing(const std::string& worker_id) {
        check_initialized();
        if (worker_id.empty())
            return 0;
        auto& redis = Cache::get().get_client();
        const std::string proc = processing_key(worker_id);
        long recovered = 0;
        // Snapshot the list, push each id back onto its type queue, then drop
        // the processing list. Not atomic across commands, but run during
        // startup only — the worker hasn't picked anything yet, so nothing
        // else writes here.
        std::vector<std::string> ids;
        try {
            redis.lrange(proc, 0, -1, std::back_inserter(ids));
        } catch (...) {
            return 0;
        }
        for (const auto& id : ids) {
            try {
                auto data = redis.get(job_key(id));
                if (!data)
                    continue;
                Job job;
                try {
                    job = Job::from_json(json::parse(*data));
                } catch (...) {
                    continue;
                }
                // Put it back on the live queue so another worker (or this one
                // after restart) picks it up again. Don't touch retry_count —
                // the job didn't fail, we just missed its completion.
                job.status = "pending";
                job.worker_id.clear();
                job.updated_at = now_epoch();
                redis.set(job_key(id), job.to_json().dump());
                redis.lpush(queue_key(job.type), id);
                ++recovered;
            } catch (...) {}
        }
        try {
            redis.del(proc);
        } catch (...) {}
        if (recovered > 0) {
            spdlog::warn("Jobs: recovered {} orphaned jobs from {}", recovered, proc);
        }
        return recovered;
    }

    /**
     * @brief Move every job whose backoff window has elapsed from the delayed
     *        set back onto its live queue. Cheap no-op when backoff is disabled.
     *        Call periodically from the worker loop.
     * @return number of jobs promoted.
     */
    long promote_due_jobs() {
        if (!initialized_ || retry_backoff_base_ms_ <= 0)
            return 0;
        auto& redis = Cache::get().get_client();
        const auto now_ms = Utils::Time::now_epoch_millis();
        std::vector<std::string> ids;
        try {
            redis.zrangebyscore(
                delayed_key(),
                sw::redis::BoundedInterval<double>(0.0, static_cast<double>(now_ms), sw::redis::BoundType::CLOSED),
                std::back_inserter(ids));
        } catch (...) {
            return 0;
        }
        long moved = 0;
        for (const auto& id : ids) {
            try {
                // ZREM-gate: only the caller that actually removes the id pushes
                // it, so concurrent workers never double-promote the same job.
                if (redis.zrem(delayed_key(), id) == 0)
                    continue;
                auto data = redis.get(job_key(id));
                if (!data)
                    continue;  // blob expired — drop the orphaned delayed entry
                auto job = Job::from_json(json::parse(*data));
                redis.lpush(queue_key(job.type), id);
                ++moved;
            } catch (...) {}
        }
        return moved;
    }

    /**
     * @brief Reclaim jobs whose visibility lease has expired — a worker picked
     *        them and died without complete()/fail(), so no same-id startup
     *        recovery will ever rescue them. Each is run back through fail()
     *        (bumps retry, requeues or DLQs), so a permanently-wedged job is not
     *        reaped forever. Cheap no-op when the lease is disabled. Call
     *        periodically from the worker loop.
     * @return number of leases reclaimed.
     */
    long reap_expired_leases() {
        if (!initialized_ || visibility_timeout_ms_ <= 0)
            return 0;
        auto& redis = Cache::get().get_client();
        const auto now_ms = Utils::Time::now_epoch_millis();
        std::vector<std::string> ids;
        try {
            redis.zrangebyscore(
                leases_key(),
                sw::redis::BoundedInterval<double>(0.0, static_cast<double>(now_ms), sw::redis::BoundType::CLOSED),
                std::back_inserter(ids));
        } catch (...) {
            return 0;
        }
        long reaped = 0;
        for (const auto& id : ids) {
            try {
                // ZREM-gate so exactly one reaper handles each expired lease.
                if (redis.zrem(leases_key(), id) == 0)
                    continue;
                fail(id, "visibility timeout exceeded");
                ++reaped;
            } catch (...) {}
        }
        return reaped;
    }

    /**
     * @brief Get job status by ID
     */
    std::optional<Job> get_status(const std::string& id) {
        check_initialized();
        auto& redis = Cache::get().get_client();

        auto data = redis.get(job_key(id));
        if (!data) {
            return std::nullopt;
        }
        return Job::from_json(json::parse(*data));
    }

    /**
     * @brief Cancel a job (sets status to failed with "cancelled" error)
     */
    bool cancel(const std::string& id) {
        check_initialized();
        auto& redis = Cache::get().get_client();

        auto data = redis.get(job_key(id));
        if (!data) {
            return false;
        }

        auto job = Job::from_json(json::parse(*data));
        // Terminal states can't be cancelled. The real vocabulary is
        // pending/processing/completed/dead (+ "failed", which only cancel()
        // itself sets). The old guard checked "failed" but not "dead", so a DLQ
        // ("dead") job could be cancelled — that TTL-expired its blob while the
        // id stayed indexed in the DLQ set, leaving a dangling entry.
        if (job.status == "completed" || job.status == "dead" || job.status == "failed") {
            return false;
        }

        const bool was_processing = (job.status == "processing");
        job.status = "failed";
        job.error = "cancelled";
        job.updated_at = now_epoch();

        redis.setex(job_key(id), result_ttl_, job.to_json().dump());

        // Drop it from wherever it currently lives: the pending queue, or — if a
        // worker already claimed it — that worker's processing list, so crash
        // recovery won't resurrect a cancelled job.
        redis.lrem(queue_key(job.type), 0, id);
        if (was_processing && !job.worker_id.empty())
            redis.lrem(processing_key(job.worker_id), 0, id);
        clear_lease_(redis, id);

        spdlog::debug("Job cancelled: id={}", id);
        return true;
    }

    /**
     * @brief One page of the time-ordered job listing plus the index total.
     */
    struct JobPage {
        std::vector<Job> jobs;
        long total = 0;
    };

    /**
     * @brief List jobs newest-first with real offset pagination, backed by
     *        the jobs:index sorted sets (score = created_at).
     * @param type Filter by job type (empty = all types).
     * @param limit Page size.
     * @param offset 0-based start position.
     * @details Index entries whose blob has expired (result_ttl) are healed
     *          lazily: dropped from the queried index as pages touch them,
     *          so `total` may transiently overcount until old pages are
     *          visited. Jobs submitted before the index existed are not
     *          listed (they age out within result_ttl anyway).
     */
    JobPage list_paged(const std::string& type = "", int limit = 20, int offset = 0) {
        check_initialized();
        auto& redis = Cache::get().get_client();
        const std::string key = type.empty() ? index_key() : index_key_for(type);

        std::vector<std::string> ids;
        redis.zrevrange(key, offset, offset + limit - 1, std::back_inserter(ids));

        JobPage page;
        if (!ids.empty()) {
            std::vector<std::string> keys;
            keys.reserve(ids.size());
            for (const auto& id : ids)
                keys.push_back(job_key(id));
            std::vector<sw::redis::OptionalString> vals;
            vals.reserve(keys.size());
            redis.mget(keys.begin(), keys.end(), std::back_inserter(vals));

            std::vector<std::string> stale;
            for (size_t i = 0; i < vals.size(); ++i) {
                if (!vals[i]) {
                    stale.push_back(ids[i]);
                    continue;
                }
                try {
                    page.jobs.push_back(Job::from_json(json::parse(*vals[i])));
                } catch (...) {
                    stale.push_back(ids[i]);
                }
            }
            // Heal: blob expired → drop the id from the queried index (and,
            // when the type is known, from the global one too; global-query
            // staleness in typed indexes heals on their own queries).
            for (const auto& id : stale) {
                try {
                    redis.zrem(key, id);
                    if (!type.empty())
                        redis.zrem(index_key(), id);
                } catch (...) {}
            }
        }
        page.total = static_cast<long>(redis.zcard(key));
        return page;
    }

    /**
     * @brief Record the worker's processing-span trace id on the job blob so
     *        UIs can deep-link into the tracing backend. Best-effort no-op
     *        when the job is gone.
     */
    void set_trace_id(const std::string& id, const std::string& trace_id) {
        check_initialized();
        auto& redis = Cache::get().get_client();
        auto data = redis.get(job_key(id));
        if (!data)
            return;
        try {
            auto job = Job::from_json(json::parse(*data));
            job.trace_id = trace_id;
            job.updated_at = now_epoch();
            redis.set(job_key(id), job.to_json().dump());
        } catch (...) {}
    }

    /**
     * @brief Health check — ping Redis
     */
    bool health_check() {
        if (!initialized_)
            return false;
        try {
            Cache::get().get_client().ping();
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Jobs health check failed: {}", e.what());
            return false;
        }
    }

    // Ordering: worker_main joins its threads before Core::shutdown() reaches
    // Jobs::shutdown(). Even so, pick() holds a local shared_ptr copy of
    // blocking_client_ for its BRPOP, so dropping the manager's reference here
    // can't free the client mid-pop — the worst case is a benign refcount race,
    // not a use-after-free on the Redis object.
    void shutdown() {
        if (initialized_) {
            spdlog::info("Shutting down job queue");
            blocking_client_.reset();
            initialized_ = false;
        }
    }

    bool is_initialized() const { return initialized_; }
    long result_ttl() const { return result_ttl_; }
    int default_max_retries() const { return default_max_retries_; }

private:
    void check_initialized() const {
        if (!initialized_) {
            throw std::runtime_error("JobQueue not initialized");
        }
    }

    // Drop a job's visibility lease (no-op when the feature is off or the lease
    // is already gone). Called from every terminal/transition path so the reaper
    // never resurrects a job that already moved on.
    void clear_lease_(sw::redis::Redis& redis, const std::string& id) {
        if (visibility_timeout_ms_ <= 0)
            return;
        try {
            redis.zrem(leases_key(), id);
        } catch (...) {}
    }

    // Backoff delay for a job that just failed its @p retry_count-th attempt
    // (>=1). Exponential (base * 2^(retry_count-1)), capped at the configured
    // max. Returns 0 when backoff is disabled → caller requeues immediately.
    int64_t backoff_delay_ms_(int retry_count) const {
        if (retry_backoff_base_ms_ <= 0)
            return 0;
        int shift = retry_count > 0 ? retry_count - 1 : 0;
        if (shift > 30)
            shift = 30;  // guard the shift against overflow
        int64_t delay = retry_backoff_base_ms_ << shift;
        if (delay <= 0 || delay > retry_backoff_max_ms_)
            delay = retry_backoff_max_ms_;
        return delay;
    }

    /**
     * @brief Load the Job rows for @p ids in a single MGET round-trip.
     *        Missing or unparseable entries are skipped silently.
     */
    std::vector<Job> fetch_jobs_by_ids_(const std::vector<std::string>& ids) {
        std::vector<Job> jobs;
        if (ids.empty())
            return jobs;
        auto& redis = Cache::get().get_client();
        std::vector<std::string> keys;
        keys.reserve(ids.size());
        for (const auto& id : ids)
            keys.push_back(job_key(id));
        std::vector<sw::redis::OptionalString> vals;
        vals.reserve(keys.size());
        redis.mget(keys.begin(), keys.end(), std::back_inserter(vals));
        jobs.reserve(vals.size());
        for (const auto& v : vals) {
            if (!v)
                continue;
            try {
                jobs.push_back(Job::from_json(json::parse(*v)));
            } catch (...) {}
        }
        return jobs;
    }
};

/**
 * @brief Global job queue instance
 */
inline std::unique_ptr<JobQueue> global_jobs = nullptr;

inline void initialize(long result_ttl = 86400, int max_retries = 3) {
    if (global_jobs != nullptr) {
        throw std::runtime_error("JobQueue already initialized");
    }
    global_jobs = std::make_unique<JobQueue>();
    global_jobs->initialize(result_ttl, max_retries);
}

inline JobQueue& get() {
    if (global_jobs == nullptr) {
        throw std::runtime_error("JobQueue not initialized");
    }
    return *global_jobs;
}

inline bool is_initialized() {
    return global_jobs != nullptr && global_jobs->is_initialized();
}

inline void shutdown() {
    if (global_jobs) {
        global_jobs->shutdown();
        global_jobs.reset();
    }
}

}  // namespace Jobs
