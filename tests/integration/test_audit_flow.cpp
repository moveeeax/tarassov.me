/**
 * @file test_audit_flow.cpp
 * @brief Integration tests for AuditController (GET /api/admin/audit): the row
 *        an admin mutation writes is readable back, filters work, and the
 *        dedicated kAuditRead permission bit gates access (a plain user is 403).
 */

#include <fstream>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AdminController.hpp"
#include "api/AuditController.hpp"
#include "api/AuthController.hpp"
#include "domain/Role.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

class AuditFlowTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AdminController admin;
    Api::AuditController audit;

    std::string config_file_name() const override { return "audit_flow_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = "test-jwt-secret-for-audit-flow-padding";
        cfg["mail"]["enabled"] = false;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("TRUNCATE TABLE audit_log");
            return 0;
        });
    }

    struct Pair {
        Domain::User user;
        Security::Auth::AuthPrincipal principal;
    };

    Pair seed_user(const std::string& email, const std::string& role_name) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name(role_name);
        if (!role) {
            ADD_FAILURE() << "role " << role_name << " missing";
            throw std::runtime_error("seed role missing");
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        Pair p;
        p.user = created;
        p.principal.subject = created.id;
        p.principal.raw_claims = json{{"sub", created.id}, {"permissions", role->permissions}};
        return p;
    }
};

TEST_F(AuditFlowTest, AdminMutationIsReadableViaAuditApi) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    // Generate an audit row through a real admin mutation.
    HttpResponsePtr created;
    admin.createUser(
        TestHelpers::authed_json(admin_user.principal, {{"email", "new@example.com"}, {"password", "password1"}}),
        [&](const HttpResponsePtr& r) { created = r; });
    ASSERT_EQ(created->statusCode(), k201Created);

    HttpResponsePtr resp;
    audit.listAudit(TestHelpers::authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_GE(body["total"].get<long>(), 1);
    bool found = false;
    for (const auto& e : body["data"])
        if (e["action"] == "user.create" && e["target_type"] == "user")
            found = true;
    EXPECT_TRUE(found) << "user.create row not returned by the audit API";
}

TEST_F(AuditFlowTest, ActionFilterNarrowsResults) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    HttpResponsePtr created;
    admin.createUser(
        TestHelpers::authed_json(admin_user.principal, {{"email", "f@example.com"}, {"password", "password1"}}),
        [&](const HttpResponsePtr& r) { created = r; });
    ASSERT_EQ(created->statusCode(), k201Created);

    // A non-matching action filter must return zero rows.
    auto req = TestHelpers::authed(admin_user.principal);
    req->setParameter("action", "role.delete");
    HttpResponsePtr resp;
    audit.listAudit(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"].get<long>(), 0);
    EXPECT_TRUE(body["data"].empty());
}

TEST_F(AuditFlowTest, PlainUserLacksAuditReadPermission) {
    auto user = seed_user("user@example.com", "User");  // permissions=0x01, no kAuditRead
    HttpResponsePtr resp;
    audit.listAudit(TestHelpers::authed(user.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}
