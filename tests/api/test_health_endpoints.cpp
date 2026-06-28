#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Api.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

// ---------------------------------------------------------------------------
// Tests that need a fully-booted Core (skip when sidecars are unavailable).
// ---------------------------------------------------------------------------

class HealthEndpointsTest : public TestHelpers::CoreBackedTest {
protected:
    Api::HealthController controller;

    std::string config_file_name() const override { return "health_test_config.json"; }
};

TEST_F(HealthEndpointsTest, ReadinessReady) {
    auto req = TestHelpers::make_request();
    HttpResponsePtr captured;

    controller.readiness(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k200OK);

    auto body = json::parse(std::string(captured->body()));
    EXPECT_EQ(body["status"], "ready");
}

TEST_F(HealthEndpointsTest, HealthDetailed) {
    auto req = TestHelpers::make_request();
    HttpResponsePtr captured;

    controller.health(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k200OK);

    auto body = json::parse(std::string(captured->body()));
    EXPECT_EQ(body["status"], "healthy");
    EXPECT_TRUE(body.contains("version"));
    EXPECT_TRUE(body.contains("components"));
    EXPECT_TRUE(body["components"].contains("database"));
    EXPECT_TRUE(body["components"].contains("cache"));
    EXPECT_TRUE(body["components"]["database"]["healthy"].get<bool>());
    EXPECT_TRUE(body["components"]["cache"]["healthy"].get<bool>());
}

// ---------------------------------------------------------------------------
// Tests that deliberately run WITHOUT Core — plain TESTs so they execute
// even in environments with no Postgres/Redis sidecars.
// ---------------------------------------------------------------------------

TEST(HealthEndpointsNoCoreTest, Liveness) {
    TestHelpers::reset_all_globals();
    Api::HealthController controller;

    auto req = TestHelpers::make_request();
    HttpResponsePtr captured;
    controller.liveness(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k200OK);

    auto body = json::parse(std::string(captured->body()));
    EXPECT_EQ(body["status"], "alive");
    EXPECT_TRUE(body.contains("timestamp"));
}

TEST(HealthEndpointsNoCoreTest, ReadinessNotReady) {
    TestHelpers::reset_all_globals();
    Api::HealthController controller;

    auto req = TestHelpers::make_request();
    HttpResponsePtr captured;
    controller.readiness(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k503ServiceUnavailable);

    auto body = json::parse(std::string(captured->body()));
    EXPECT_EQ(body["status"], "not_ready");
}

TEST(HealthEndpointsNoCoreTest, HealthUnhealthyWithNoSubsystems) {
    TestHelpers::reset_all_globals();
    Api::HealthController controller;

    auto req = TestHelpers::make_request();
    HttpResponsePtr captured;
    controller.health(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k503ServiceUnavailable);

    auto body = json::parse(std::string(captured->body()));
    EXPECT_EQ(body["status"], "unhealthy");
    EXPECT_EQ(body["version"], "unknown");
}
