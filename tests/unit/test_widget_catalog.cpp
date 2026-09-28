/**
 * @file test_widget_catalog.cpp
 * @brief The widget catalog is the single list the API validates against and
 *        publishes. Pure logic, no Postgres — runs in the unit bucket.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Validation.hpp"
#include "domain/Role.hpp"
#include "domain/WidgetCatalog.hpp"

using json = nlohmann::json;
namespace W = Domain::Widgets;

namespace {

json valid_widget(const char* type = "posts_summary") {
    return json{{"widget_type", type}, {"grid_x", 0}, {"grid_y", 0}, {"grid_w", 4}, {"grid_h", 3}};
}

// Every catalog entry an admin may place, as the controller's own filter sees it.
bool admin_permits(std::uint32_t /*perm*/) {
    return true;
}

TEST(WidgetCatalog, FindsKnownTypeAndRejectsUnknown) {
    const auto* spec = W::find("posts_summary");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->min_w, 3);
    EXPECT_EQ(W::find("no_such_widget"), nullptr);
}

TEST(WidgetCatalog, CatalogJsonFiltersByPermission) {
    // A caller holding only kAuditRead sees exactly the audit widget.
    auto only_audit = [](std::uint32_t perm) { return perm == Domain::Permission::kAuditRead; };
    auto filtered = W::catalog_json(only_audit);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0]["type"], "audit_recent");

    auto all = W::catalog_json(admin_permits);
    EXPECT_EQ(all.size(), 5u);
    EXPECT_TRUE(all[0].contains("default_w"));
    EXPECT_TRUE(all[0]["options"].is_object());
}

TEST(WidgetCatalog, AcceptsValidWidget) {
    Api::Validation::Errors errs;
    W::validate_widget(errs, valid_widget(), *W::find("posts_summary"), 0);
    EXPECT_FALSE(errs.any());
}

TEST(WidgetCatalog, RejectsNonIntegerGeometry) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_x"] = 1.5;
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[0].grid_x");
    EXPECT_EQ(errs.items()[0].code, "not_integer");
}

TEST(WidgetCatalog, RejectsNegativeGeometry) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_y"] = -1;
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "below_min");
}

TEST(WidgetCatalog, RejectsMissingGeometry) {
    Api::Validation::Errors errs;
    json w = json{{"widget_type", "posts_summary"}, {"grid_x", 0}};
    W::validate_widget(errs, w, *W::find("posts_summary"), 2);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[2].grid_y");
    EXPECT_EQ(errs.items()[0].code, "missing");
}

TEST(WidgetCatalog, RejectsWidthBelowSpecMinimum) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_w"] = 1;  // posts_summary declares min_w = 3
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[0].grid_w");
}

TEST(WidgetCatalog, RejectsWidgetRunningPastTheGrid) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["grid_x"] = 10;
    w["grid_w"] = 4;  // 10 + 4 > 12
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "out_of_grid");
}

TEST(WidgetCatalog, RejectsUnknownOptionKey) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["options"] = json{{"colour", 3}};
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].field, "widgets[0].options.colour");
    EXPECT_EQ(errs.items()[0].code, "unknown_option");
}

TEST(WidgetCatalog, RejectsOptionOutOfRange) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["options"] = json{{"limit", 999}};  // posts_summary caps limit at 10
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "above_max");
}

TEST(WidgetCatalog, RejectsNonObjectOptions) {
    Api::Validation::Errors errs;
    auto w = valid_widget();
    w["options"] = "limit=5";
    W::validate_widget(errs, w, *W::find("posts_summary"), 0);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "not_object");
}

}  // namespace
