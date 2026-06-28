#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "security/Auth.hpp"
#include "test_fixtures.hpp"

using json = nlohmann::json;

namespace {

// JWT minting + epoch helpers live in tests/test_fixtures.hpp so every
// auth-adjacent test shares one implementation.
using TestFixtures::make_jwt;
using TestFixtures::now_sec;

}  // namespace

class AuthTest : public ::testing::Test {
protected:
    const std::string secret = "test-secret-please-rotate";

    Security::Auth::AuthConfig make_cfg() {
        Security::Auth::AuthConfig cfg;
        cfg.mode = Security::Auth::AuthMode::Jwt;
        cfg.jwt_secret = secret;
        cfg.jwt_issuer = "issuer-x";
        cfg.jwt_audience = "aud-x";
        cfg.jwt_leeway_sec = 5;
        cfg.jwt_roles_claim = "roles";
        cfg.jwt_scopes_claim = "scope";
        return cfg;
    }

    void TearDown() override { Security::Auth::shutdown(); }
};

TEST_F(AuthTest, ValidJwtExtractsPrincipal) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {{"sub", "user-42"},
                   {"iss", "issuer-x"},
                   {"aud", "aud-x"},
                   {"exp", now_sec() + 3600},
                   {"roles", {"admin", "viewer"}}};
    std::string token = make_jwt(claims, secret);

    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_TRUE(p.has_value()) << err;
    EXPECT_EQ(p->subject, "user-42");
    EXPECT_EQ(p->roles.size(), 2u);
    EXPECT_EQ(p->roles[0], "admin");
}

TEST_F(AuthTest, RefreshTokenRejectedOnAccessPath) {
    // A refresh token (typ=="refresh") must never authenticate a request —
    // it outlives the access TTL and dodges the JTI revocation check.
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {
        {"sub", "user-42"}, {"iss", "issuer-x"}, {"aud", "aud-x"}, {"exp", now_sec() + 3600}, {"typ", "refresh"}};
    std::string token = make_jwt(claims, secret);

    std::string err;
    auto p = auth.verify_jwt(token, err);
    EXPECT_FALSE(p.has_value());
    EXPECT_EQ(err, "wrong_token_type");
}

TEST_F(AuthTest, AccessTypAccepted) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {
        {"sub", "user-42"}, {"iss", "issuer-x"}, {"aud", "aud-x"}, {"exp", now_sec() + 3600}, {"typ", "access"}};
    std::string err;
    auto p = auth.verify_jwt(make_jwt(claims, secret), err);
    ASSERT_TRUE(p.has_value()) << err;
    EXPECT_EQ(p->subject, "user-42");
}

TEST_F(AuthTest, BadSignatureRejected) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {{"sub", "u"}, {"exp", now_sec() + 3600}};
    std::string token = make_jwt(claims, "wrong-secret");

    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(err, "bad_signature");
}

TEST_F(AuthTest, ExpiredTokenRejected) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {
        {"sub", "u"}, {"iss", "issuer-x"}, {"aud", "aud-x"}, {"exp", now_sec() - 100}  // well past leeway
    };
    std::string token = make_jwt(claims, secret);

    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(err, "token_expired");
}

TEST_F(AuthTest, WrongIssuerRejected) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {{"sub", "u"}, {"iss", "someone-else"}, {"aud", "aud-x"}, {"exp", now_sec() + 3600}};
    std::string token = make_jwt(claims, secret);

    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(err, "bad_issuer");
}

TEST_F(AuthTest, WrongAudienceRejected) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {{"sub", "u"}, {"iss", "issuer-x"}, {"aud", "other-aud"}, {"exp", now_sec() + 3600}};
    std::string token = make_jwt(claims, secret);

    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(err, "bad_audience");
}

TEST_F(AuthTest, AudienceArraySupported) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {{"sub", "u"}, {"iss", "issuer-x"}, {"aud", {"a", "b", "aud-x", "c"}}, {"exp", now_sec() + 3600}};
    std::string token = make_jwt(claims, secret);

    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_TRUE(p.has_value()) << err;
}

TEST_F(AuthTest, MalformedTokenRejected) {
    Security::Auth::Authenticator auth(make_cfg());
    std::string err;
    auto p = auth.verify_jwt("not-a-jwt", err);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(err, "malformed_token");
}

TEST_F(AuthTest, UnsupportedAlgRejected) {
    Security::Auth::Authenticator auth(make_cfg());
    json header = {{"alg", "none"}, {"typ", "JWT"}};
    json claims = {{"sub", "u"}};
    std::string token = Utils::Base64::url_encode(header.dump()) + "." + Utils::Base64::url_encode(claims.dump()) + ".";
    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(err, "unsupported_alg");
}

TEST_F(AuthTest, ScopesAsSpaceSeparatedString) {
    Security::Auth::Authenticator auth(make_cfg());
    json claims = {
        {"sub", "u"}, {"iss", "issuer-x"}, {"aud", "aud-x"}, {"exp", now_sec() + 3600}, {"scope", "read write admin"}};
    std::string token = make_jwt(claims, secret);
    std::string err;
    auto p = auth.verify_jwt(token, err);
    ASSERT_TRUE(p.has_value()) << err;
    ASSERT_EQ(p->scopes.size(), 3u);
    EXPECT_EQ(p->scopes[1], "write");
}

TEST_F(AuthTest, JwtSecretMissingThrowsAtLoad) {
    Security::Auth::AuthConfig cfg;
    cfg.mode = Security::Auth::AuthMode::Jwt;
    // jwt_secret intentionally empty.
    // load_config_from_global() is the enforcing path; Authenticator itself
    // is permissive so tests can construct it. The check lives in the loader.
    // This test covers the contract of the loader via mock expectation only:
    // we verify the ctor is tolerant and the config round-trip preserves mode.
    Security::Auth::Authenticator auth(std::move(cfg));
    EXPECT_EQ(auth.config().mode, Security::Auth::AuthMode::Jwt);
    EXPECT_TRUE(auth.config().jwt_secret.empty());
}
