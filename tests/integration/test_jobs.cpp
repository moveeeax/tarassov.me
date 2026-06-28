/**
 * @file test_jobs.cpp
 * @brief Integration tests for JobQueue: submit / pick / complete / fail-retry / cancel.
 * @details Requires live Redis. Tests cover the retry path and the cancel
 *          semantics (completed/failed jobs cannot be cancelled). The TOCTOU
 *          race on cancel() is documented with a DISABLED_ test — once cancel()
 *          grows atomic semantics the test can be re-enabled.
 */

#include <atomic>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "jobs/Jobs.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class JobsIntegrationTest : public TestHelpers::CoreBackedTest {
protected:
    // Queue names used by the tests below; TearDown drains exactly these.
    static constexpr const char* kQueues[] = {
        "default", "retryq", "cancelq", "pagedq", "healq", "depthq", "backoffq", "leaseq"};

    std::vector<std::string> job_ids_to_cleanup;

    std::string config_file_name() const override { return "jobs_int_test_config.json"; }

    void config_overrides(nlohmann::json& config) override {
        config["logging"]["name"] = "jobs_int_test";
        config["logging"]["file"] = "logs/jobs_int_test.log";
        config["observability"]["service_name"] = "jobs_int_svc";
        config["database"]["migrations_enabled"] = false;
        config["jobs"]["enabled"] = true;
        config["jobs"]["result_ttl"] = 3600;
        config["jobs"]["max_retries"] = 2;
    }

    // In-process tests don't go through worker_main, which is where
    // production normally calls init_blocking_client(). Without it,
    // pick()'s BRPOPLPUSH falls back to the Cache pool whose 500ms
    // socket_timeout is shorter than the 2s BRPOP argument and raises
    // "Resource temporarily unavailable" under load. Give the test a
    // dedicated blocking client so the timeout budgets match.
    void post_init() override {
        Jobs::get().init_blocking_client(
            TestHelpers::redis_host(), TestHelpers::redis_port(), /*brpop_timeout_sec=*/5, /*password=*/"");
    }

    void TearDown() override {
        if (Cache::is_initialized()) {
            for (const auto& id : job_ids_to_cleanup) {
                try {
                    Cache::get().del(Jobs::job_key(id));
                    Cache::get().get_client().zrem(Jobs::index_key(), id);
                } catch (...) {}
            }
            TestHelpers::drain_jobs({std::begin(kQueues), std::end(kQueues)});
            // The backoff / visibility-timeout ZSETs are global (not per-type),
            // so drain_jobs doesn't touch them — clear them here to keep tests
            // isolated.
            try {
                Cache::get().get_client().del(Jobs::delayed_key());
                Cache::get().get_client().del(Jobs::leases_key());
            } catch (...) {}
        }
        TestHelpers::CoreBackedTest::TearDown();
    }

    void track(const std::string& id) { job_ids_to_cleanup.push_back(id); }
};

TEST_F(JobsIntegrationTest, SubmitPickComplete) {
    auto job = Jobs::get().submit("default", {{"n", 1}});
    track(job.id);
    EXPECT_EQ(job.status, "pending");

    auto picked = Jobs::get().pick({"default"}, 2, "w1");
    ASSERT_TRUE(picked);
    EXPECT_EQ(picked->id, job.id);
    EXPECT_EQ(picked->status, "processing");
    EXPECT_EQ(picked->worker_id, "w1");

    Jobs::get().complete(job.id, {{"ok", true}});
    auto final_state = Jobs::get().get_status(job.id);
    ASSERT_TRUE(final_state);
    EXPECT_EQ(final_state->status, "completed");
    EXPECT_EQ(final_state->result.at("ok").get<bool>(), true);
}

TEST_F(JobsIntegrationTest, RecoverProcessingRequeuesLeftover) {
    auto job = Jobs::get().submit("recoverq", {{"n", 7}});
    track(job.id);
    // Claim onto a per-thread processing list (the worker-N-i id form from #15).
    auto picked = Jobs::get().pick({"recoverq"}, 2, "rwrk-0");
    ASSERT_TRUE(picked);
    ASSERT_EQ(picked->id, job.id);

    // Simulate a crash before completion: the id is stranded in rwrk-0's
    // processing list. recover_processing must push it back onto its queue —
    // the exact silent-job-loss path #15 fixed, previously untested.
    long recovered = Jobs::get().recover_processing("rwrk-0");
    EXPECT_EQ(recovered, 1);

    // Back on its queue and pickable again.
    auto again = Jobs::get().pick({"recoverq"}, 2, "rwrk-0");
    ASSERT_TRUE(again);
    EXPECT_EQ(again->id, job.id);
}

TEST_F(JobsIntegrationTest, FailRetriesThenPermanentFail) {
    // max_retries=2 from config
    auto job = Jobs::get().submit("retryq", {});
    track(job.id);

    // First attempt fails — should requeue.
    auto p1 = Jobs::get().pick({"retryq"}, 2, "w1");
    ASSERT_TRUE(p1);
    Jobs::get().fail(job.id, "boom-1");
    auto after1 = Jobs::get().get_status(job.id);
    ASSERT_TRUE(after1);
    EXPECT_EQ(after1->status, "pending");
    EXPECT_EQ(after1->retry_count, 1);

    // Second pick should re-pop the same job.
    auto p2 = Jobs::get().pick({"retryq"}, 2, "w2");
    ASSERT_TRUE(p2);
    EXPECT_EQ(p2->id, job.id);

    // Second failure reaches max_retries → moved to the dead-letter queue.
    Jobs::get().fail(job.id, "boom-2");
    auto after2 = Jobs::get().get_status(job.id);
    ASSERT_TRUE(after2);
    EXPECT_EQ(after2->status, "dead");
    EXPECT_EQ(after2->retry_count, 2);
    EXPECT_EQ(after2->error, "boom-2");

    // The job should be visible in the DLQ listing.
    auto dlq = Jobs::get().list_dlq("retryq", 10);
    bool found_in_dlq = false;
    for (const auto& j : dlq)
        if (j.id == job.id) {
            found_in_dlq = true;
            break;
        }
    EXPECT_TRUE(found_in_dlq);

    // Requeuing resets the state and pushes it back onto the live queue.
    EXPECT_TRUE(Jobs::get().requeue_from_dlq(job.id));
    auto after_requeue = Jobs::get().get_status(job.id);
    ASSERT_TRUE(after_requeue);
    EXPECT_EQ(after_requeue->status, "pending");
    EXPECT_EQ(after_requeue->retry_count, 0);
}

TEST_F(JobsIntegrationTest, CancelPendingJob) {
    auto job = Jobs::get().submit("cancelq", {});
    track(job.id);

    EXPECT_TRUE(Jobs::get().cancel(job.id));

    auto status = Jobs::get().get_status(job.id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->status, "failed");
    EXPECT_EQ(status->error, "cancelled");

    // Queue entry must have been removed so a worker doesn't pick a zombie.
    // Use LRANGE directly — BRPOP via pick() here would run against the Cache
    // pool's 100ms socket_timeout and raise a redis Error.
    std::vector<std::string> queue_contents;
    Cache::get().get_client().lrange(Jobs::queue_key("cancelq"), 0, -1, std::back_inserter(queue_contents));
    EXPECT_TRUE(queue_contents.empty());
}

TEST_F(JobsIntegrationTest, CancelCompletedJobReturnsFalse) {
    auto job = Jobs::get().submit("cancelq", {});
    track(job.id);

    auto picked = Jobs::get().pick({"cancelq"}, 2, "w1");
    ASSERT_TRUE(picked);
    Jobs::get().complete(job.id, {{"ok", true}});

    EXPECT_FALSE(Jobs::get().cancel(job.id));

    auto status = Jobs::get().get_status(job.id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->status, "completed");
}

TEST_F(JobsIntegrationTest, CancelUnknownJobReturnsFalse) {
    EXPECT_FALSE(Jobs::get().cancel("00000000-0000-0000-0000-000000000000"));
}

// TOCTOU on cancel(): read-decide-write is not atomic, so two concurrent
// cancellers can both observe a pending job and both write "failed" state.
// This test exists to document the race — disabled until cancel() becomes
// atomic (e.g. via Redis WATCH/MULTI or a Lua script).
TEST_F(JobsIntegrationTest, DISABLED_CancelIsAtomicUnderContention) {
    auto job = Jobs::get().submit("cancelq", {});
    track(job.id);

    std::atomic<int> successes{0};
    auto race = [&]() {
        if (Jobs::get().cancel(job.id))
            ++successes;
    };
    std::thread t1(race), t2(race);
    t1.join();
    t2.join();

    // Atomic cancel should allow exactly one successful cancellation.
    EXPECT_EQ(successes.load(), 1);
}

TEST_F(JobsIntegrationTest, ListPagedSlicesWithExactTotal) {
    std::set<std::string> submitted;
    for (int i = 0; i < 5; ++i) {
        auto j = Jobs::get().submit("pagedq", {{"n", i}});
        track(j.id);
        submitted.insert(j.id);
    }

    auto p1 = Jobs::get().list_paged("pagedq", 2, 0);
    auto p2 = Jobs::get().list_paged("pagedq", 2, 2);
    auto p3 = Jobs::get().list_paged("pagedq", 2, 4);

    EXPECT_EQ(p1.total, 5);
    EXPECT_EQ(p1.jobs.size(), 2u);
    EXPECT_EQ(p2.jobs.size(), 2u);
    EXPECT_EQ(p3.jobs.size(), 1u);

    // The three pages partition the submitted set — no dup, no loss.
    // (Same-second submits share a score, so per-page order is tie-broken
    // lexicographically; only the partition property is guaranteed.)
    std::set<std::string> seen;
    for (const auto* page : {&p1, &p2, &p3})
        for (const auto& j : page->jobs)
            seen.insert(j.id);
    EXPECT_EQ(seen, submitted);
}

TEST_F(JobsIntegrationTest, ListPagedHealsExpiredIndexEntries) {
    auto j1 = Jobs::get().submit("healq", {{"k", 1}});
    auto j2 = Jobs::get().submit("healq", {{"k", 2}});
    track(j1.id);
    track(j2.id);

    // Simulate the blob expiring via result_ttl while the index entry stays.
    Cache::get().del(Jobs::job_key(j1.id));

    auto page = Jobs::get().list_paged("healq", 10, 0);
    ASSERT_EQ(page.jobs.size(), 1u);
    EXPECT_EQ(page.jobs[0].id, j2.id);

    // The stale id was healed out of the index: total is exact now.
    auto again = Jobs::get().list_paged("healq", 10, 0);
    EXPECT_EQ(again.total, 1);
}

TEST_F(JobsIntegrationTest, QueueDepthByTypeCountsWaiting) {
    // Measure our own delta rather than assume a clean slate.
    const auto before = Jobs::get().queue_depth_by_type();
    const long base = before.count("depthq") ? before.at("depthq") : 0;

    for (int i = 0; i < 3; ++i) {
        auto j = Jobs::get().submit("depthq", {{"n", i}});
        track(j.id);
    }

    auto depth = Jobs::get().queue_depth_by_type();
    ASSERT_TRUE(depth.count("depthq"));
    EXPECT_EQ(depth.at("depthq"), base + 3);

    // Picking a job off the waiting queue drops the depth by one.
    auto picked = Jobs::get().pick({"depthq"}, 2, "w1");
    ASSERT_TRUE(picked);
    auto after = Jobs::get().queue_depth_by_type();
    EXPECT_EQ(after.count("depthq") ? after.at("depthq") : 0, base + 2);
}

TEST_F(JobsIntegrationTest, SetTraceIdPersistsOnBlob) {
    auto j = Jobs::get().submit("default", {{"x", 1}});
    track(j.id);

    const std::string trace = "0123456789abcdef0123456789abcdef";
    Jobs::get().set_trace_id(j.id, trace);

    auto status = Jobs::get().get_status(j.id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->trace_id, trace);
}

TEST_F(JobsIntegrationTest, BackoffDelaysRequeueUntilDue) {
    Jobs::get().set_retry_backoff(/*base_ms=*/200, /*max_ms=*/1000);

    auto job = Jobs::get().submit("backoffq", {});
    track(job.id);

    auto p1 = Jobs::get().pick({"backoffq"}, 2, "w1");
    ASSERT_TRUE(p1);
    Jobs::get().fail(job.id, "boom");

    // Retry was counted and the job stays "pending", but with backoff enabled it
    // is parked in the delayed set, NOT immediately back on the live queue.
    auto after = Jobs::get().get_status(job.id);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status, "pending");
    EXPECT_EQ(after->retry_count, 1);

    std::vector<std::string> q;
    Cache::get().get_client().lrange(Jobs::queue_key("backoffq"), 0, -1, std::back_inserter(q));
    EXPECT_TRUE(q.empty());

    // Before the delay elapses, promotion moves nothing.
    EXPECT_EQ(Jobs::get().promote_due_jobs(), 0);

    // After the backoff window, the job is promoted back and pickable again.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(Jobs::get().promote_due_jobs(), 1);

    auto p2 = Jobs::get().pick({"backoffq"}, 2, "w1");
    ASSERT_TRUE(p2);
    EXPECT_EQ(p2->id, job.id);
}

TEST_F(JobsIntegrationTest, VisibilityTimeoutReapsExpiredLease) {
    Jobs::get().set_visibility_timeout(/*sec=*/100);  // pick() now records a lease

    auto job = Jobs::get().submit("leaseq", {});
    track(job.id);

    auto p1 = Jobs::get().pick({"leaseq"}, 2, "wkr-0");
    ASSERT_TRUE(p1);

    // Simulate a worker that picked the job and died without completing it:
    // backdate its lease so it's already expired.
    Cache::get().get_client().zadd(Jobs::leases_key(), job.id, 1.0);

    // The reaper reclaims the orphaned job: treated as a failure (retry bumped),
    // dropped from the dead worker's processing list, and requeued.
    EXPECT_EQ(Jobs::get().reap_expired_leases(), 1);

    auto after = Jobs::get().get_status(job.id);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->retry_count, 1);

    std::vector<std::string> proc;
    Cache::get().get_client().lrange(Jobs::processing_key("wkr-0"), 0, -1, std::back_inserter(proc));
    EXPECT_TRUE(proc.empty());

    // Backoff is disabled in this test → fail() requeues immediately, so another
    // worker can pick it up.
    auto p2 = Jobs::get().pick({"leaseq"}, 2, "wkr-1");
    ASSERT_TRUE(p2);
    EXPECT_EQ(p2->id, job.id);
}

TEST_F(JobsIntegrationTest, CompleteClearsVisibilityLease) {
    Jobs::get().set_visibility_timeout(/*sec=*/100);

    auto job = Jobs::get().submit("leaseq", {});
    track(job.id);

    auto p1 = Jobs::get().pick({"leaseq"}, 2, "wkr-0");
    ASSERT_TRUE(p1);
    // Lease recorded at pick. (redis-plus-plus Optional has operator bool, not
    // std::optional's has_value().)
    EXPECT_TRUE(static_cast<bool>(Cache::get().get_client().zscore(Jobs::leases_key(), job.id)));

    Jobs::get().complete(job.id, {{"ok", true}});

    // Completing the job clears its lease so the reaper can't resurrect it.
    EXPECT_FALSE(static_cast<bool>(Cache::get().get_client().zscore(Jobs::leases_key(), job.id)));
    EXPECT_EQ(Jobs::get().reap_expired_leases(), 0);
}

}  // namespace
