/**
 * @file test_http_e2e.cpp
 * @brief End-to-end tests over REAL HTTP.
 *
 * Every other suite drives controller methods directly, which means the
 * middleware chain registered in Api::register_controllers() — auth gate,
 * content-type check, idempotency, tracing headers, Drogon routing, cookie
 * serialization on the wire — never executes in tests. This binary closes
 * that gap: it boots Core, registers the controllers, runs the real Drogon
 * server on a loopback port in a background thread, and talks to it with
 * drogon::HttpClient.
 *
 * Built as a SEPARATE executable (tarassov_me_e2e): drogon::app() is a
 * process-wide singleton whose run()/quit() cycle is once-per-process, so it
 * cannot coexist with suites that reset global state between tests.
 *
 * Requires the Postgres + Redis sidecars (same as the integration bucket).
 * Run inside Docker:  make test-e2e
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Api.hpp"
#include "core/Core.hpp"
#include "domain/Role.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "e2e-test-jwt-secret-0123456789-abcdef";  // >=32 chars (Auth boot guard)
constexpr uint16_t kPort = 18098;

bool g_env_ok = false;

std::string base_url() {
    return "http://127.0.0.1:" + std::to_string(kPort);
}

/**
 * Boots Core + Drogon exactly once for the whole binary. Tests skip when
 * the sidecars are unreachable (g_env_ok stays false).
 */
class HttpServerEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        std::filesystem::create_directories("logs");
        if (!TestHelpers::is_postgres_available() || !TestHelpers::is_redis_available()) {
            return;  // tests will GTEST_SKIP
        }

        json cfg = json::parse(TestHelpers::minimal_config());
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["auth"]["cookies"]["enabled"] = true;
        cfg["auth"]["cookies"]["secure"] = false;  // plain http in the test net
        cfg["idempotency"]["enabled"] = true;
        cfg["mail"]["enabled"] = false;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";

        config_path_ = TestHelpers::create_temp_config(cfg.dump(2), "e2e_test_config.json");
        Core::initialize(config_path_);

        TestHelpers::truncate_users();

        Api::register_controllers();
        app().addListener("127.0.0.1", kPort).setThreadNum(1);
        server_thread_ = std::thread([] { app().run(); });

        // Once the server thread is running, any exception thrown before we
        // return (e.g. the HTTP client throwing under a slow/loaded runner)
        // would escape SetUp — gtest then destroys the fixture WITHOUT calling
        // TearDown, and ~std::thread on a still-joinable server_thread_ calls
        // std::terminate ("terminate called without an active exception").
        // Stop and join the thread before letting any exception propagate.
        try {
            // Wait until the server actually accepts: poll /healthz. A request
            // sent before the listener is up can THROW (connection refused)
            // rather than return an error result, so catch per-attempt and keep
            // polling instead of aborting SetUp on a transient startup race.
            auto client = HttpClient::newHttpClient(base_url());
            for (int i = 0; i < 100; ++i) {
                try {
                    auto req = HttpRequest::newHttpRequest();
                    req->setPath("/healthz");
                    auto [ok, resp] = client->sendRequest(req, /*timeout=*/1.0);
                    if (ok == ReqResult::Ok && resp && resp->statusCode() == k200OK) {
                        g_env_ok = true;
                        return;
                    }
                } catch (...) {
                    // server not accepting yet — fall through to the retry sleep
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ADD_FAILURE() << "e2e server did not become ready on " << base_url();
        } catch (...) {
            // Defense in depth: never let server_thread_ be destroyed joinable
            // (that calls std::terminate). Stop and join before propagating.
            app().quit();
            if (server_thread_.joinable())
                server_thread_.join();
            throw;
        }
    }

    void TearDown() override {
        if (server_thread_.joinable()) {
            app().quit();
            server_thread_.join();
        }
        TestHelpers::reset_all_globals();
        TestHelpers::remove_temp_config(config_path_);
    }

private:
    std::string config_path_;
    std::thread server_thread_;
};

#define REQUIRE_E2E_ENV() \
    if (!g_env_ok)        \
    GTEST_SKIP() << "Postgres/Redis sidecars unavailable — e2e server not started"

// ---------------------------------------------------------------------------
// Small sync-request helpers.
// ---------------------------------------------------------------------------

HttpResponsePtr send(const HttpRequestPtr& req) {
    auto client = HttpClient::newHttpClient(base_url());
    auto [ok, resp] = client->sendRequest(req, /*timeout=*/5.0);
    EXPECT_EQ(ok, ReqResult::Ok) << "transport error talking to e2e server";
    EXPECT_NE(resp, nullptr);
    return resp;
}

HttpRequestPtr json_post(const std::string& path, const json& body) {
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath(path);
    req->setBody(body.dump());
    req->setContentTypeCode(CT_APPLICATION_JSON);
    return req;
}

json body_of(const HttpResponsePtr& resp) {
    return json::parse(std::string(resp->getBody()));
}

struct SessionCookies {
    std::string access;
    std::string refresh;
};

SessionCookies cookies_of(const HttpResponsePtr& resp) {
    SessionCookies out;
    for (const auto& [name, c] : resp->getCookies()) {
        if (name.find("access") != std::string::npos)
            out.access = c.value();
        else if (name.find("refresh") != std::string::npos)
            out.refresh = c.value();
    }
    return out;
}

void attach_session(const HttpRequestPtr& req, const SessionCookies& sc) {
    const auto& cookie_cfg = Security::Auth::get().config().cookies;
    if (!sc.access.empty())
        req->addCookie(cookie_cfg.access_name, sc.access);
    if (!sc.refresh.empty())
        req->addCookie(cookie_cfg.refresh_name, sc.refresh);
}

SessionCookies register_and_login(const std::string& email, const std::string& password) {
    auto reg = send(json_post("/api/v1/auth/register", {{"email", email}, {"password", password}}));
    EXPECT_EQ(reg->statusCode(), k201Created) << reg->getBody();
    auto login = send(json_post("/api/v1/auth/login", {{"email", email}, {"password", password}}));
    EXPECT_EQ(login->statusCode(), k200OK) << login->getBody();
    return cookies_of(login);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(HttpE2E, HealthzCarriesRequestIdHeader) {
    REQUIRE_E2E_ENV();
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/healthz");
    auto resp = send(req);
    EXPECT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(body_of(resp)["status"], "alive");
    // Tracing middleware must stamp every response.
    EXPECT_FALSE(resp->getHeader("x-request-id").empty());
}

TEST(HttpE2E, SecurityHeadersStampedOnEveryResponse) {
    REQUIRE_E2E_ENV();
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/healthz");
    auto resp = send(req);
    EXPECT_EQ(resp->getHeader("x-content-type-options"), "nosniff");
    EXPECT_EQ(resp->getHeader("x-frame-options"), "DENY");
    EXPECT_EQ(resp->getHeader("referrer-policy"), "no-referrer");
    // API responses are JSON — CSP is locked down (default-src 'none').
    EXPECT_FALSE(resp->getHeader("content-security-policy").empty());
}

TEST(HttpE2E, TraceparentPropagatesToResponse) {
    REQUIRE_E2E_ENV();
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/healthz");
    req->addHeader("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    auto resp = send(req);
    EXPECT_EQ(resp->getHeader("x-request-id"), "0af7651916cd43dd8448eb211c80319c");
}

TEST(HttpE2E, NonJsonContentTypeRejectedWith415) {
    REQUIRE_E2E_ENV();
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/api/v1/auth/login");
    req->setBody("email=not-json");
    req->setContentTypeCode(CT_TEXT_PLAIN);
    auto resp = send(req);
    EXPECT_EQ(resp->statusCode(), k415UnsupportedMediaType);
}

TEST(HttpE2E, AuthMiddlewareGuardsNonPublicPaths) {
    REQUIRE_E2E_ENV();
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/api/v1/jobs");  // not in api.public_paths
    auto resp = send(req);
    EXPECT_EQ(resp->statusCode(), k401Unauthorized);
    EXPECT_FALSE(resp->getHeader("www-authenticate").empty());
}

TEST(HttpE2E, AccountTokenRoutesArePublic) {
    // The token-bearing account routes must reach their handler WITHOUT a
    // session (the user clicking an email link isn't logged in). Pre-fix the
    // wildcard public path was missing and the auth middleware 401'd here,
    // breaking confirm/reset/change-email end-to-end. We expect the handler's
    // own 400 invalid_token (bad token), NOT a 401 from the middleware.
    REQUIRE_E2E_ENV();
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    req->setPath("/api/v1/account/reset-password/not-a-real-token");
    req->setBody(R"({"new_password":"whatever-123"})");
    req->setContentTypeCode(CT_APPLICATION_JSON);  // pass the content-type gate
    auto resp = send(req);
    EXPECT_EQ(resp->statusCode(), k400BadRequest) << resp->getBody();
    EXPECT_EQ(body_of(resp)["error"], "invalid_token");
}

TEST(HttpE2E, RegisterLoginMeRoundtripOverWire) {
    REQUIRE_E2E_ENV();
    auto sc = register_and_login("e2e-alice@example.com", "password-e2e-1");
    ASSERT_FALSE(sc.access.empty()) << "no access cookie on the wire";
    ASSERT_FALSE(sc.refresh.empty()) << "no refresh cookie on the wire";

    auto me = HttpRequest::newHttpRequest();
    me->setPath("/api/v1/auth/me");
    attach_session(me, sc);
    auto resp = send(me);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->getBody();
    EXPECT_EQ(body_of(resp)["user"]["email"], "e2e-alice@example.com");
}

TEST(HttpE2E, RefreshRotatesSession) {
    REQUIRE_E2E_ENV();
    auto sc = register_and_login("e2e-bob@example.com", "password-e2e-1");

    auto refresh = HttpRequest::newHttpRequest();
    refresh->setMethod(Post);
    refresh->setPath("/api/v1/auth/refresh");
    attach_session(refresh, sc);
    auto resp = send(refresh);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->getBody();

    auto rotated = cookies_of(resp);
    EXPECT_FALSE(rotated.refresh.empty());
    EXPECT_NE(rotated.refresh, sc.refresh) << "refresh token must rotate";
}

TEST(HttpE2E, LogoutRevokesRefreshToken) {
    REQUIRE_E2E_ENV();
    auto sc = register_and_login("e2e-carol@example.com", "password-e2e-1");

    auto logout = HttpRequest::newHttpRequest();
    logout->setMethod(Post);
    logout->setPath("/api/v1/auth/logout");
    attach_session(logout, sc);
    ASSERT_EQ(send(logout)->statusCode(), k200OK);

    // The old refresh JTI is revoked in Redis — rotation must now fail.
    auto refresh = HttpRequest::newHttpRequest();
    refresh->setMethod(Post);
    refresh->setPath("/api/v1/auth/refresh");
    attach_session(refresh, sc);
    EXPECT_EQ(send(refresh)->statusCode(), k401Unauthorized);
}

TEST(HttpE2E, IdempotencyKeyReplaysResponse) {
    REQUIRE_E2E_ENV();
    const json body = {{"email", "e2e-idem@example.com"}, {"password", "password-e2e-1"}};

    auto first = json_post("/api/v1/auth/register", body);
    first->addHeader("Idempotency-Key", "e2e-key-001");
    auto r1 = send(first);
    ASSERT_EQ(r1->statusCode(), k201Created) << r1->getBody();

    // Identical retry: without the middleware this would be 409 email_taken;
    // with it, the cached 201 is replayed.
    auto second = json_post("/api/v1/auth/register", body);
    second->addHeader("Idempotency-Key", "e2e-key-001");
    auto r2 = send(second);
    EXPECT_EQ(r2->statusCode(), k201Created) << r2->getBody();
    EXPECT_EQ(r2->getHeader("x-idempotent-replayed"), "true");

    // Same key + DIFFERENT body → 422 conflict.
    auto third =
        json_post("/api/v1/auth/register", {{"email", "e2e-other@example.com"}, {"password", "password-e2e-1"}});
    third->addHeader("Idempotency-Key", "e2e-key-001");
    EXPECT_EQ(send(third)->statusCode(), k422UnprocessableEntity);
}

TEST(HttpE2E, AdminGateChecksPermissionBitmask) {
    REQUIRE_E2E_ENV();
    const auto now = Utils::Time::now_epoch_seconds();

    json admin_claims = {
        {"sub", "e2e-admin"}, {"iat", now}, {"exp", now + 600}, {"permissions", Domain::Permission::kAdminister}};
    json user_claims = {{"sub", "e2e-user"}, {"iat", now}, {"exp", now + 600}, {"permissions", 1}};
    const auto admin_jwt = Security::Auth::issue_hs256_jwt(admin_claims, kSecret);
    const auto user_jwt = Security::Auth::issue_hs256_jwt(user_claims, kSecret);

    auto as_admin = HttpRequest::newHttpRequest();
    as_admin->setPath("/api/v1/admin/users");
    as_admin->addHeader("Authorization", "Bearer " + admin_jwt);
    EXPECT_EQ(send(as_admin)->statusCode(), k200OK);

    auto as_user = HttpRequest::newHttpRequest();
    as_user->setPath("/api/v1/admin/users");
    as_user->addHeader("Authorization", "Bearer " + user_jwt);
    EXPECT_EQ(send(as_user)->statusCode(), k403Forbidden);
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new HttpServerEnvironment);
    return RUN_ALL_TESTS();
}
