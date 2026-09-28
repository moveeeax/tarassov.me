#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Api.hpp"
#include "api/Endpoints.hpp"
#include "security/Auth.hpp"
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

// The SPA can only reach the backend through nginx's `location /api/`
// (frontend/nginx.conf), which the spec freezes. So the detailed probe the
// dashboard's service widget reads must be registered under /api/v1 as well —
// /health alone falls through to the SPA fallback and answers index.html.
TEST(HealthRoutes, DetailedHealthIsAlsoRegisteredUnderApiV1) {
    bool found = false;
    for (const auto& ep : Api::get_endpoints())
        if (ep.method == "GET" && ep.path == "/api/v1/health")
            found = true;
    EXPECT_TRUE(found) << "GET /api/v1/health missing from Api::get_endpoints()";
}

// ---------- /api/v1/health is the browser-reachable alias, so it is gated ----------

class HealthApiV1GateTest : public TestHelpers::CoreBackedTest {
protected:
    Api::HealthController controller;

    std::string config_file_name() const override { return "health_api_v1_gate_test_config.json"; }

    // The gate only exists when auth is on: API_REQUIRE_ADMIN is a no-op under
    // AUTH_MODE=none, so with the default config this suite would pass vacuously.
    void config_overrides(nlohmann::json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = "test-jwt-secret-for-health-api-v1-gate-padding";
        cfg["mail"]["enabled"] = false;
    }
};

TEST_F(HealthApiV1GateTest, AnonymousCallerIsRefused) {
    HttpResponsePtr captured;
    controller.healthAuthed(TestHelpers::make_request(), [&](const HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k403Forbidden);
}

TEST_F(HealthApiV1GateTest, AdminGetsTheSamePayloadAsTheBareProbe) {
    Security::Auth::AuthPrincipal admin;
    admin.subject = "00000000-0000-0000-0000-000000000001";
    admin.raw_claims = json{{"sub", admin.subject}, {"permissions", 0x40000000u}};

    HttpResponsePtr captured;
    controller.healthAuthed(TestHelpers::authed(admin), [&](const HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    // 200 or 503 depending on component health; what matters is that the gate let
    // the admin through and the payload is the probe's, not an error envelope.
    EXPECT_NE(captured->statusCode(), k403Forbidden);
    auto body = json::parse(std::string(captured->body()));
    EXPECT_TRUE(body.contains("version"));
    EXPECT_TRUE(body.contains("components"));
}
