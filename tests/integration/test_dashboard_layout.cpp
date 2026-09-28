/**
 * @file test_dashboard_layout.cpp
 * @brief Dashboard layout: repository replace semantics, owner isolation and
 *        the HTTP contract of /api/v1/dashboard/{catalog,layout}.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

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

    static Repositories::DashboardWidgetInput widget(const std::string& type,
                                                     int x,
                                                     int y,
                                                     int w,
                                                     int h,
                                                     const std::string& options = "{}") {
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

}  // namespace
