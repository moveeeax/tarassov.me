/**
 * @file test_require_confirmed.cpp
 * @brief Unit tests for Security::Auth::require_confirmed — the confirmed-email
 *        guard (#21). Three outcomes: anonymous→401, unconfirmed→403, confirmed
 *        →pass. Uses the Auth DI test-seam so no Postgres/Redis is needed.
 */

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "security/Auth.hpp"

class RequireConfirmedTest : public ::testing::Test {
protected:
    void SetUp() override {
        Security::Auth::AuthConfig cfg;
        cfg.mode = Security::Auth::AuthMode::Jwt;                 // != None, so the guard actually runs
        cfg.jwt_secret = "test-secret-please-rotate-0123456789";  // >= 32 chars
        Security::Auth::install_for_testing(std::move(cfg));
    }
    void TearDown() override { Security::Auth::shutdown(); }

    static drogon::HttpRequestPtr req_with_confirmed(bool confirmed) {
        Security::Auth::AuthPrincipal p;
        p.subject = "user-1";
        p.raw_claims = nlohmann::json{{"confirmed", confirmed}};
        auto req = drogon::HttpRequest::newHttpRequest();
        req->attributes()->insert(Security::Auth::kPrincipalAttr, p);
        return req;
    }
};

TEST_F(RequireConfirmedTest, AnonymousIsUnauthorized) {
    auto resp = Security::Auth::require_confirmed(drogon::HttpRequest::newHttpRequest());
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), drogon::k401Unauthorized);
}

TEST_F(RequireConfirmedTest, UnconfirmedIsForbidden) {
    auto resp = Security::Auth::require_confirmed(req_with_confirmed(false));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), drogon::k403Forbidden);
}

TEST_F(RequireConfirmedTest, ConfirmedPasses) {
    auto resp = Security::Auth::require_confirmed(req_with_confirmed(true));
    EXPECT_EQ(resp, nullptr);  // nullptr == allowed through
}
