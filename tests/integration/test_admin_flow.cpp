/**
 * @file test_admin_flow.cpp
 * @brief Integration tests for AdminController.
 *
 * Exercises the require_admin guard (anonymous → 403, admin → ok) and
 * the self-protection rules (no self-delete, no self-role-change).
 */

#include <chrono>
#include <filesystem>
#include <fstream>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AdminController.hpp"
#include "api/AuthController.hpp"
#include "domain/Role.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-admin-flow-padding";

// Multi-bit masks used by the Roles-CRUD tests. Built from the named
// Domain::Permission bits so a backend bit-layout change shows up here.
constexpr std::uint32_t kEditorPerms = Domain::Permission::kGeneral | 0x02;                // 0x03
constexpr std::uint32_t kEditorPermsWidened = Domain::Permission::kGeneral | 0x02 | 0x04;  // 0x07

class AdminFlowTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AuthController auth;
    Api::AdminController admin;

    std::string config_file_name() const override { return "admin_flow_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["auth"]["cookies"]["enabled"] = true;
        cfg["auth"]["cookies"]["secure"] = false;
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
            // Drop extra roles inserted by prior test bodies (e.g. "Editor")
            // so create() can re-add them. The two seed roles from migration
            // 001 (ON CONFLICT DO NOTHING) are kept stable.
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            return 0;
        });
    }

    /// Create a user with the named role and return both the row and a
    /// pre-built principal that callers can stuff into req->attributes().
    struct Pair {
        Domain::User user;
        Security::Auth::AuthPrincipal principal;
    };

    Pair seed_user(const std::string& email, const std::string& role_name) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name(role_name);
        if (!role) {
            // ASSERT_* is unusable in a value-returning helper, and EXPECT
            // followed by role->id would dereference an empty optional (UB).
            ADD_FAILURE() << "role " << role_name << " missing — seed migration?";
            throw std::runtime_error("seed role missing: " + role_name);
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        auto fresh = users.find(created.id);
        Pair p;
        p.user = fresh ? *fresh : created;
        p.principal.subject = p.user.id;
        if (p.user.role)
            p.principal.roles.push_back(p.user.role->name);
        // Mirror what AuthController::mint_session would put in claims.
        p.principal.raw_claims = json{{"sub", p.user.id}, {"permissions", p.user.role ? p.user.role->permissions : 0u}};
        return p;
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p) { return TestHelpers::authed(p); }

    static HttpRequestPtr authed_post(const Security::Auth::AuthPrincipal& p, const json& body) {
        return TestHelpers::authed_json(p, body);
    }
};

TEST_F(AdminFlowTest, listUsersForbiddenForRegularUser) {
    auto regular = seed_user("user@example.com", "User");
    HttpResponsePtr resp;
    admin.listUsers(authed(regular.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(AdminFlowTest, listUsersWorksForAdmin) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    seed_user("u1@example.com", "User");
    seed_user("u2@example.com", "User");
    HttpResponsePtr resp;
    admin.listUsers(authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_GE(body["total"].get<long>(), 3);
    EXPECT_TRUE(body["data"].is_array());
}

TEST_F(AdminFlowTest, createUserAsAdmin) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    auto req = authed_post(admin_user.principal, {{"email", "fresh@example.com"}, {"password", "password1"}});
    HttpResponsePtr resp;
    admin.createUser(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("fresh@example.com");
    ASSERT_TRUE(user.has_value());
    EXPECT_TRUE(user->confirmed);  // admin-created users are pre-confirmed
}

TEST_F(AdminFlowTest, createUserWritesAuditLog) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    auto req = authed_post(admin_user.principal, {{"email", "audited@example.com"}, {"password", "password1"}});
    HttpResponsePtr resp;
    admin.createUser(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto created = repo.find_by_email("audited@example.com");
    ASSERT_TRUE(created.has_value());

    // Audit::record swallows exceptions, so a broken INSERT would otherwise pass
    // silently — assert the trail row actually landed, stamped with the actor.
    auto rows = Database::get().execute_read([&](auto& txn) {
        return txn.exec_params(
            "SELECT actor_id, action FROM audit_log "
            "WHERE action = 'user.create' AND target_id = $1",
            created->id);
    });
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0]["actor_id"].template as<std::string>(), admin_user.user.id);
}

TEST_F(AdminFlowTest, deleteUserAsAdmin) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    auto target = seed_user("victim@example.com", "User");
    HttpResponsePtr resp;
    admin.deleteUser(
        authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; }, target.user.id);
    ASSERT_EQ(resp->statusCode(), k200OK);
    Repositories::UserRepository repo;
    EXPECT_FALSE(repo.find(target.user.id).has_value());
}

TEST_F(AdminFlowTest, adminCannotDeleteSelf) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    HttpResponsePtr resp;
    admin.deleteUser(
        authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; }, admin_user.user.id);
    ASSERT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "self_delete");
    Repositories::UserRepository repo;
    EXPECT_TRUE(repo.find(admin_user.user.id).has_value());
}

TEST_F(AdminFlowTest, adminCannotChangeOwnRole) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    Repositories::RoleRepository roles;
    auto user_role = roles.find_by_name("User");
    ASSERT_TRUE(user_role.has_value());

    auto req = authed_post(admin_user.principal, {{"role_id", user_role->id}});
    req->setMethod(Patch);
    HttpResponsePtr resp;
    admin.updateUser(
        req, [&](const HttpResponsePtr& r) { resp = r; }, admin_user.user.id);
    ASSERT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "self_role_change");
}

TEST_F(AdminFlowTest, listRolesShowsBothSeededRoles) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    HttpResponsePtr resp;
    admin.listRoles(authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    auto names = std::vector<std::string>{};
    for (const auto& r : body["data"])
        names.push_back(r["name"].get<std::string>());
    EXPECT_NE(std::find(names.begin(), names.end(), "User"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "Administrator"), names.end());
}

// ── Roles CRUD ─────────────────────────────────────────────────────────────

TEST_F(AdminFlowTest, createRoleAsAdmin) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    auto req = authed_post(admin_user.principal, {{"name", "Editor"}, {"permissions", kEditorPerms}});
    HttpResponsePtr resp;
    admin.createRole(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "Editor");
    EXPECT_EQ(body["data"]["permissions"].get<int>(), 3);
}

TEST_F(AdminFlowTest, createRoleRejectsDuplicateName) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    auto req = authed_post(admin_user.principal, {{"name", "User"}, {"permissions", 1}});
    HttpResponsePtr resp;
    admin.createRole(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "role_exists");
}

TEST_F(AdminFlowTest, updateRolePatchesFields) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    Repositories::RoleRepository repo;
    auto created = repo.create("Editor", kEditorPerms, false);

    auto req = authed_post(admin_user.principal, {{"permissions", kEditorPermsWidened}});
    req->setMethod(Patch);
    HttpResponsePtr resp;
    admin.updateRole(
        req, [&](const HttpResponsePtr& r) { resp = r; }, std::to_string(created.id));
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["permissions"].get<int>(), 7);
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "Editor");
}

TEST_F(AdminFlowTest, updateRoleEmptyPatchRefused) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    Repositories::RoleRepository repo;
    auto created = repo.create("Editor", kEditorPerms, false);
    auto req = authed_post(admin_user.principal, json::object());
    req->setMethod(Patch);
    HttpResponsePtr resp;
    admin.updateRole(
        req, [&](const HttpResponsePtr& r) { resp = r; }, std::to_string(created.id));
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(AdminFlowTest, deleteRoleHappyPath) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    Repositories::RoleRepository repo;
    auto created = repo.create("Editor", kEditorPerms, false);
    HttpResponsePtr resp;
    admin.deleteRole(
        authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; }, std::to_string(created.id));
    EXPECT_EQ(resp->statusCode(), k200OK);
    EXPECT_FALSE(repo.find(created.id).has_value());
}

TEST_F(AdminFlowTest, deleteRoleProtectsDefault) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    Repositories::RoleRepository repo;
    auto user_role = repo.find_by_name("User");
    ASSERT_TRUE(user_role.has_value());
    HttpResponsePtr resp;
    admin.deleteRole(
        authed(admin_user.principal), [&](const HttpResponsePtr& r) { resp = r; }, std::to_string(user_role->id));
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "default_role_protected");
    EXPECT_TRUE(repo.find(user_role->id).has_value());
}

TEST_F(AdminFlowTest, deleteRoleRefusesIfReferenced) {
    auto admin_user = seed_user("admin@example.com", "Administrator");
    HttpResponsePtr resp;
    admin.deleteRole(
        authed(admin_user.principal),
        [&](const HttpResponsePtr& r) { resp = r; },
        std::to_string(admin_user.user.role_id));
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "role_in_use");
}

}  // namespace
