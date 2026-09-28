/**
 * @file test_dashboard_layout.cpp
 * @brief Dashboard layout: repository replace semantics, owner isolation and
 *        the HTTP contract of /api/v1/dashboard/{catalog,layout}.
 */

#include <functional>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/DashboardController.hpp"
#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "repositories/DashboardWidgetRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class DashboardLayoutTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "dashboard_layout_test_config.json"; }

    // Auth must be ON for this suite. Security::Auth::require_permission answers
    // "permitted" for everything when mode is None (the dev-mode escape the
    // controller deliberately reuses), so with the default config the catalog
    // would never be filtered and PUT would never answer 403 — the two things
    // half of these cases are about.
    void config_overrides(nlohmann::json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = "test-jwt-secret-for-dashboard-layout-padding";
        cfg["mail"]["enabled"] = false;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // CASCADE also clears dashboard_widgets (FK ON DELETE CASCADE).
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Seed a user and return the principal a handler would see. `permissions`
    /// lands in the JWT claims, which is what current_user_can reads.
    Security::Auth::AuthPrincipal seed_user(const std::string& email, std::uint32_t permissions) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        EXPECT_TRUE(role.has_value());
        auto user = users.create(email, std::string("$argon2id$x"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
        p.raw_claims = json{{"sub", user.id}, {"permissions", permissions}};
        return p;
    }

    static Repositories::DashboardWidgetInput widget(
        const std::string& type, int x, int y, int w, int h, const std::string& options = "{}") {
        Repositories::DashboardWidgetInput in;
        in.widget_type = type;
        in.grid_x = x;
        in.grid_y = y;
        in.grid_w = w;
        in.grid_h = h;
        in.options_json = options;
        return in;
    }
};

TEST_F(DashboardLayoutTest, RepositoryStoresAndReadsBackInGridOrder) {
    auto owner = seed_user("owner@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    auto stored = repo.replace_all(
        owner.subject, {widget("jobs_queue", 0, 1, 4, 3), widget("posts_summary", 0, 0, 4, 3, R"({"limit":7})")});
    ASSERT_EQ(stored.size(), 2u);

    auto read = repo.list_owned(owner.subject, 40, 0);
    ASSERT_EQ(read.size(), 2u);
    // kOrderBy is "grid_y, grid_x", so row 0 comes first regardless of insert order.
    EXPECT_EQ(read[0].widget_type, "posts_summary");
    EXPECT_EQ(read[1].widget_type, "jobs_queue");
    // options is jsonb in the table and a parsed object in the DTO.
    EXPECT_TRUE(read[0].options.is_object());
    EXPECT_EQ(read[0].options["limit"], 7);
    EXPECT_EQ(read[1].options, json::object());
}

TEST_F(DashboardLayoutTest, ReplaceAllOverwritesTheWholeSet) {
    auto owner = seed_user("replace@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    repo.replace_all(owner.subject, {widget("posts_summary", 0, 0, 4, 3), widget("jobs_queue", 4, 0, 4, 3)});
    auto second = repo.replace_all(owner.subject, {widget("service_health", 0, 0, 4, 2)});

    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(repo.count_owned(owner.subject), 1);
    EXPECT_EQ(repo.list_owned(owner.subject, 40, 0)[0].widget_type, "service_health");
}

TEST_F(DashboardLayoutTest, ReplaceAllWithEmptySetClearsTheLayout) {
    auto owner = seed_user("clear@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    repo.replace_all(owner.subject, {widget("posts_summary", 0, 0, 4, 3)});
    auto cleared = repo.replace_all(owner.subject, {});

    EXPECT_TRUE(cleared.empty());
    EXPECT_EQ(repo.count_owned(owner.subject), 0);
}

TEST_F(DashboardLayoutTest, OneOwnerNeverSeesAnothersLayout) {
    auto alice = seed_user("alice@example.com", Domain::Permission::kAdminister);
    auto bob = seed_user("bob@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;

    auto alice_rows = repo.replace_all(alice.subject, {widget("posts_summary", 0, 0, 4, 3)});
    repo.replace_all(bob.subject, {widget("jobs_queue", 0, 0, 4, 3), widget("service_health", 4, 0, 4, 2)});

    EXPECT_EQ(repo.count_owned(alice.subject), 1);
    EXPECT_EQ(repo.count_owned(bob.subject), 2);
    // find_owned is the only lookup a handler may use: Bob cannot fetch Alice's row.
    EXPECT_FALSE(repo.find_owned(alice_rows[0].id, bob.subject).has_value());
    EXPECT_TRUE(repo.find_owned(alice_rows[0].id, alice.subject).has_value());
}

TEST_F(DashboardLayoutTest, DeletingTheUserCascadesTheLayout) {
    auto owner = seed_user("cascade@example.com", Domain::Permission::kAdminister);
    Repositories::DashboardWidgetRepository repo;
    repo.replace_all(owner.subject, {widget("posts_summary", 0, 0, 4, 3)});

    Repositories::UserRepository users;
    users.remove(owner.subject);

    EXPECT_EQ(repo.count_owned(owner.subject), 0);
}

using namespace drogon;

class DashboardApiTest : public DashboardLayoutTest {
protected:
    Api::DashboardController controller;

    json get_catalog(const Security::Auth::AuthPrincipal& p, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.catalog(TestHelpers::authed(p), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    json get_layout(const Security::Auth::AuthPrincipal& p, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.layout(TestHelpers::authed(p), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    json put_layout(const Security::Auth::AuthPrincipal& p, const json& body, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.saveLayout(TestHelpers::authed_json(p, body, drogon::Put),
                              [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    static json widget_json(const char* type, int x, int y, int w, int h) {
        return json{{"widget_type", type}, {"grid_x", x}, {"grid_y", y}, {"grid_w", w}, {"grid_h", h}};
    }
};

TEST_F(DashboardApiTest, CatalogIsFilteredByPermission) {
    auto admin = seed_user("admin-cat@example.com", Domain::Permission::kAdminister);
    auto auditor = seed_user("auditor-cat@example.com", Domain::Permission::kAuditRead);

    EXPECT_EQ(get_catalog(admin)["data"].size(), 5u);

    auto limited = get_catalog(auditor)["data"];
    ASSERT_EQ(limited.size(), 1u);
    EXPECT_EQ(limited[0]["type"], "audit_recent");
}

TEST_F(DashboardApiTest, LayoutStartsEmptyAndRoundTrips) {
    auto owner = seed_user("roundtrip@example.com", Domain::Permission::kAdminister);
    EXPECT_TRUE(get_layout(owner)["data"].empty());

    int status = 0;
    auto saved = put_layout(
        owner,
        json{{"widgets",
              json::array({widget_json("posts_summary", 0, 0, 4, 3), widget_json("jobs_queue", 4, 0, 4, 3)})}},
        &status);
    ASSERT_EQ(status, k200OK);
    ASSERT_EQ(saved["data"].size(), 2u);
    EXPECT_TRUE(saved["data"][0].contains("id"));
    // owner_id never leaves the DTO.
    EXPECT_FALSE(saved["data"][0].contains("owner_id"));

    auto read = get_layout(owner)["data"];
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0]["widget_type"], "posts_summary");
    EXPECT_EQ(read[0]["options"], json::object());
}

TEST_F(DashboardApiTest, EmptyWidgetArrayClearsTheLayout) {
    auto owner = seed_user("empty@example.com", Domain::Permission::kAdminister);
    put_layout(owner, json{{"widgets", json::array({widget_json("posts_summary", 0, 0, 4, 3)})}});

    int status = 0;
    auto cleared = put_layout(owner, json{{"widgets", json::array()}}, &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_TRUE(cleared["data"].empty());
    EXPECT_TRUE(get_layout(owner)["data"].empty());
}

TEST_F(DashboardApiTest, TwoWidgetsOfTheSameTypeAreAllowed) {
    auto owner = seed_user("twins@example.com", Domain::Permission::kAdminister);
    auto a = widget_json("posts_summary", 0, 0, 4, 3);
    a["options"] = json{{"limit", 3}};
    auto b = widget_json("posts_summary", 4, 0, 4, 3);
    b["options"] = json{{"limit", 10}};

    int status = 0;
    auto saved = put_layout(owner, json{{"widgets", json::array({a, b})}}, &status);
    ASSERT_EQ(status, k200OK);
    ASSERT_EQ(saved["data"].size(), 2u);
    EXPECT_EQ(saved["data"][0]["options"]["limit"], 3);
    EXPECT_EQ(saved["data"][1]["options"]["limit"], 10);
}

TEST_F(DashboardApiTest, RejectsUnknownWidgetType) {
    auto owner = seed_user("unknown@example.com", Domain::Permission::kAdminister);
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", json::array({widget_json("nope", 0, 0, 4, 3)})}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["error"], "validation_failed");
    EXPECT_EQ(body["errors"][0]["field"], "widgets[0].widget_type");
    EXPECT_EQ(body["errors"][0]["code"], "unknown_widget");
}

TEST_F(DashboardApiTest, RejectsFractionalGeometryWithA400NotA500) {
    auto owner = seed_user("fraction@example.com", Domain::Permission::kAdminister);
    auto w = widget_json("posts_summary", 0, 0, 4, 3);
    w["grid_x"] = 1.5;
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", json::array({w})}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["field"], "widgets[0].grid_x");
    EXPECT_EQ(body["errors"][0]["code"], "not_integer");
}

TEST_F(DashboardApiTest, RejectsOptionOutOfRange) {
    auto owner = seed_user("range@example.com", Domain::Permission::kAdminister);
    auto w = widget_json("posts_summary", 0, 0, 4, 3);
    w["options"] = json{{"limit", 999}};
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", json::array({w})}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["code"], "above_max");
}

TEST_F(DashboardApiTest, RejectsMoreThanFortyWidgets) {
    auto owner = seed_user("flood@example.com", Domain::Permission::kAdminister);
    json widgets = json::array();
    for (int i = 0; i < 41; ++i)
        widgets.push_back(widget_json("service_health", 0, i, 4, 2));
    int status = 0;
    auto body = put_layout(owner, json{{"widgets", widgets}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["code"], "too_many");
}

TEST_F(DashboardApiTest, RejectsMissingWidgetsArray) {
    auto owner = seed_user("nobody@example.com", Domain::Permission::kAdminister);
    int status = 0;
    auto body = put_layout(owner, json{{"layout", json::array()}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["errors"][0]["field"], "widgets");
}

TEST_F(DashboardApiTest, PlacingAWidgetWithoutItsPermissionIs403) {
    auto auditor = seed_user("auditor-put@example.com", Domain::Permission::kAuditRead);
    int status = 0;
    // posts_summary needs kAdminister; the auditor holds only kAuditRead.
    put_layout(auditor, json{{"widgets", json::array({widget_json("posts_summary", 0, 0, 4, 3)})}}, &status);
    EXPECT_EQ(status, k403Forbidden);

    // Its own widget still goes through.
    int ok_status = 0;
    put_layout(auditor, json{{"widgets", json::array({widget_json("audit_recent", 0, 0, 6, 4)})}}, &ok_status);
    EXPECT_EQ(ok_status, k200OK);
}

TEST_F(DashboardApiTest, LayoutsAreIsolatedPerCaller) {
    auto alice = seed_user("alice-api@example.com", Domain::Permission::kAdminister);
    auto bob = seed_user("bob-api@example.com", Domain::Permission::kAdminister);

    put_layout(alice, json{{"widgets", json::array({widget_json("posts_summary", 0, 0, 4, 3)})}});
    EXPECT_TRUE(get_layout(bob)["data"].empty());

    put_layout(bob, json{{"widgets", json::array({widget_json("jobs_queue", 0, 0, 4, 3)})}});
    EXPECT_EQ(get_layout(alice)["data"].size(), 1u);
    EXPECT_EQ(get_layout(alice)["data"][0]["widget_type"], "posts_summary");
}

}  // namespace
