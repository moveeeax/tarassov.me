#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Api.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

// ---------- JobsController Tests ----------

class JobsEndpointTest : public TestHelpers::CoreBackedTest {
protected:
    Api::JobsController controller;
    std::vector<std::string> job_ids_to_cleanup;

    std::string config_file_name() const override { return "jobs_test_config.json"; }

    void config_overrides(nlohmann::json& cfg) override {
        cfg["jobs"]["enabled"] = true;
        cfg["jobs"]["result_ttl"] = 3600;
        cfg["jobs"]["max_retries"] = 3;
    }

    void TearDown() override {
        // Shared drain: blobs + queue/dlq/index keys. The old per-id-only
        // cleanup leaked queue entries and jobs:index members between runs.
        if (Cache::is_initialized()) {
            for (const auto& id : job_ids_to_cleanup) {
                try {
                    Cache::get().del(Jobs::job_key(id));
                    Cache::get().get_client().zrem(Jobs::index_key(), id);
                } catch (...) {}
            }
            TestHelpers::drain_jobs({"echo"});
        }
        TestHelpers::CoreBackedTest::TearDown();
    }

    /// Submit a job through the controller and return the parsed response body.
    json submit(const json& payload) {
        HttpResponsePtr resp;
        controller.submitJob(TestHelpers::post_json(payload), [&](const HttpResponsePtr& r) { resp = r; });
        EXPECT_NE(resp, nullptr);
        json body = json::parse(std::string(resp->body()));
        if (body.contains("data") && body["data"].contains("id"))
            job_ids_to_cleanup.push_back(body["data"]["id"].get<std::string>());
        body["_status"] = static_cast<int>(resp->statusCode());
        return body;
    }
};

TEST_F(JobsEndpointTest, SubmitJob) {
    auto body = submit({{"type", "echo"}, {"payload", {{"msg", "hello"}}}});
    EXPECT_EQ(body["_status"], 201);
    EXPECT_EQ(body["message"], "Job submitted");
    EXPECT_TRUE(body["data"].contains("id"));
    EXPECT_EQ(body["data"]["status"], "pending");
    EXPECT_EQ(body["data"]["type"], "echo");
}

TEST_F(JobsEndpointTest, SubmitJobWithMaxRetries) {
    auto body = submit({{"type", "echo"}, {"payload", json::object()}, {"max_retries", 5}});
    EXPECT_EQ(body["_status"], 201);
    EXPECT_EQ(body["data"]["max_retries"], 5);
}

TEST_F(JobsEndpointTest, SubmitJobInvalidJson) {
    auto req = TestHelpers::make_request(Post);
    req->setBody("not valid json{{{");

    HttpResponsePtr captured;
    controller.submitJob(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k400BadRequest);
}

TEST_F(JobsEndpointTest, SubmitJobMissingType) {
    HttpResponsePtr captured;
    controller.submitJob(TestHelpers::post_json({{"payload", {{"key", "val"}}}}),
                         [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k400BadRequest);
}

TEST_F(JobsEndpointTest, SubmitJobEmptyType) {
    HttpResponsePtr captured;
    controller.submitJob(TestHelpers::post_json({{"type", ""}}), [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k400BadRequest);
}

TEST_F(JobsEndpointTest, GetJobStatus) {
    auto submit_body = submit({{"type", "echo"}, {"payload", {{"x", 1}}}});
    ASSERT_EQ(submit_body["_status"], 201);
    std::string job_id = submit_body["data"]["id"];

    HttpResponsePtr get_resp;
    controller.getJobStatus(
        TestHelpers::make_request(), [&](const HttpResponsePtr& resp) { get_resp = resp; }, job_id);

    ASSERT_NE(get_resp, nullptr);
    EXPECT_EQ(get_resp->statusCode(), k200OK);

    auto get_body = json::parse(std::string(get_resp->body()));
    EXPECT_EQ(get_body["data"]["id"], job_id);
    EXPECT_EQ(get_body["data"]["status"], "pending");
    EXPECT_EQ(get_body["data"]["type"], "echo");
}

TEST_F(JobsEndpointTest, GetJobStatusNotFound) {
    HttpResponsePtr captured;
    controller.getJobStatus(
        TestHelpers::make_request(),
        [&](const HttpResponsePtr& resp) { captured = resp; },
        "00000000-0000-0000-0000-000000000000");

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k404NotFound);
}

TEST_F(JobsEndpointTest, GetJobStatusInvalidUUID) {
    HttpResponsePtr captured;
    controller.getJobStatus(
        TestHelpers::make_request(), [&](const HttpResponsePtr& resp) { captured = resp; }, "not-a-uuid");

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k400BadRequest);
}

TEST_F(JobsEndpointTest, CancelJob) {
    auto submit_body = submit({{"type", "slow"}, {"payload", {{"seconds", 10}}}});
    ASSERT_EQ(submit_body["_status"], 201);
    std::string job_id = submit_body["data"]["id"];

    HttpResponsePtr cancel_resp;
    controller.cancelJob(
        TestHelpers::make_request(Delete), [&](const HttpResponsePtr& resp) { cancel_resp = resp; }, job_id);

    ASSERT_NE(cancel_resp, nullptr);
    EXPECT_EQ(cancel_resp->statusCode(), k200OK);

    auto cancel_body = json::parse(std::string(cancel_resp->body()));
    EXPECT_EQ(cancel_body["message"], "Job cancelled");

    // Verify status is failed/cancelled
    HttpResponsePtr get_resp;
    controller.getJobStatus(
        TestHelpers::make_request(), [&](const HttpResponsePtr& resp) { get_resp = resp; }, job_id);
    auto get_body = json::parse(std::string(get_resp->body()));
    EXPECT_EQ(get_body["data"]["status"], "failed");
    EXPECT_EQ(get_body["data"]["error"], "cancelled");
}

TEST_F(JobsEndpointTest, CancelJobNotFound) {
    HttpResponsePtr captured;
    controller.cancelJob(
        TestHelpers::make_request(Delete),
        [&](const HttpResponsePtr& resp) { captured = resp; },
        "00000000-0000-0000-0000-000000000000");

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k404NotFound);
}

TEST_F(JobsEndpointTest, CancelJobInvalidUUID) {
    HttpResponsePtr captured;
    controller.cancelJob(
        TestHelpers::make_request(Delete), [&](const HttpResponsePtr& resp) { captured = resp; }, "bad-uuid");

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k400BadRequest);
}

TEST_F(JobsEndpointTest, ListJobs) {
    for (int i = 0; i < 2; ++i) {
        auto body = submit({{"type", "echo"}, {"payload", {{"i", i}}}});
        ASSERT_EQ(body["_status"], 201);
    }

    HttpResponsePtr list_resp;
    controller.listJobs(TestHelpers::make_request(), [&](const HttpResponsePtr& resp) { list_resp = resp; });

    ASSERT_NE(list_resp, nullptr);
    EXPECT_EQ(list_resp->statusCode(), k200OK);

    auto list_body = json::parse(std::string(list_resp->body()));
    EXPECT_TRUE(list_body["data"].is_array());
    EXPECT_GE(list_body["total"].get<int>(), 2);
    EXPECT_EQ(list_body["limit"].get<int>(), 20);
    EXPECT_EQ(list_body["offset"].get<int>(), 0);
}

TEST_F(JobsEndpointTest, ListJobsPaginatesWithOffset) {
    for (int i = 0; i < 3; ++i) {
        auto body = submit({{"type", "echo"}, {"payload", {{"i", i}}}});
        ASSERT_EQ(body["_status"], 201);
    }

    auto page_req = TestHelpers::make_request();
    page_req->setParameter("limit", "2");
    page_req->setParameter("offset", "2");
    page_req->setParameter("type", "echo");
    HttpResponsePtr page_resp;
    controller.listJobs(page_req, [&](const HttpResponsePtr& resp) { page_resp = resp; });

    ASSERT_NE(page_resp, nullptr);
    EXPECT_EQ(page_resp->statusCode(), k200OK);
    auto body = json::parse(std::string(page_resp->body()));
    EXPECT_EQ(body["limit"].get<int>(), 2);
    EXPECT_EQ(body["offset"].get<int>(), 2);
    EXPECT_GE(body["total"].get<int>(), 3);
    EXPECT_GE(body["data"].size(), 1u);
}
