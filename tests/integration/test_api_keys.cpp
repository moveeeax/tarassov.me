/**
 * @file test_api_keys.cpp
 * @brief API key lifecycle: create (secret once) → authenticate → list (no
 *        secret) → revoke (stops auth) → owner-scoping.
 */

#include <functional>
#include <string>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/ApiKeyController.hpp"
#include "database/Database.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/ApiKeys.hpp"
#include "test_helpers.hpp"

using namespace drogon;
using json = nlohmann::json;

namespace {

class ApiKeysTest : public TestHelpers::CoreBackedTest {
protected:
    Api::ApiKeyController controller;

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // CASCADE also clears api_keys (FK ON DELETE CASCADE).
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    Security::Auth::AuthPrincipal seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        EXPECT_TRUE(role.has_value());
        auto u = users.create(email, std::string("$argon2id$x"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = u.id;
        p.raw_claims = json{{"sub", u.id}, {"permissions", role ? role->permissions : 0u}};
        return p;
    }

    json create_key(const Security::Auth::AuthPrincipal& p, const std::string& name, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.create(TestHelpers::authed_json(p, {{"name", name}}), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    static HttpRequestPtr with_key(const std::string& key) {
        auto req = HttpRequest::newHttpRequest();
        req->addHeader("X-API-Key", key);
        return req;
    }
};

TEST_F(ApiKeysTest, CreateReturnsSecretOnceThenAuthenticates) {
    auto user = seed_user("keyuser@example.com");
    int status = 0;
    auto body = create_key(user, "ci-token", &status);
    ASSERT_EQ(status, k201Created);

    const std::string key = body["key"];
    EXPECT_EQ(key.rfind("cpk_", 0), 0u);  // has the prefix
    EXPECT_EQ(body["prefix"].get<std::string>().rfind("cpk_", 0), 0u);
    EXPECT_FALSE(body.contains("key_hash"));  // never leaks the hash

    auto principal = Security::ApiKeys::authenticate(with_key(key));
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->subject, user.subject);
}

TEST_F(ApiKeysTest, ListHidesSecretAndRevokeStopsAuth) {
    auto user = seed_user("keyuser2@example.com");
    auto created = create_key(user, "t");
    const std::string key = created["key"];
    const std::string id = created["id"];

    HttpResponsePtr listResp;
    controller.list(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { listResp = r; });
    auto list = json::parse(std::string(listResp->body()));
    ASSERT_EQ(list["total"], 1);
    EXPECT_FALSE(list["data"][0].contains("key"));
    EXPECT_FALSE(list["data"][0].contains("key_hash"));

    HttpResponsePtr delResp;
    controller.remove(
        TestHelpers::authed(user), [&](const HttpResponsePtr& r) { delResp = r; }, id);
    EXPECT_EQ(delResp->statusCode(), k200OK);

    EXPECT_FALSE(Security::ApiKeys::authenticate(with_key(key)).has_value());  // revoked
}

TEST_F(ApiKeysTest, RevokeIsOwnerScoped) {
    auto alice = seed_user("alice@example.com");
    auto bob = seed_user("bob@example.com");
    const std::string id = create_key(alice, "a")["id"];

    // Bob can't revoke Alice's key → 404 (no info leak that it exists).
    HttpResponsePtr resp;
    controller.remove(
        TestHelpers::authed(bob), [&](const HttpResponsePtr& r) { resp = r; }, id);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

}  // namespace
