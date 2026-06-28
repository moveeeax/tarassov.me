/**
 * @file test_list_query.cpp
 * @brief Unit tests for the allowlist-based sort helper — the anti-injection
 *        guard for list endpoints. Pure logic, no Postgres/Redis.
 */

#include <array>
#include <string_view>

#include <gtest/gtest.h>

#include "repositories/ListQuery.hpp"

using Repositories::safe_order_by;

namespace {
constexpr std::array<std::string_view, 2> kSortable{"created_at", "name"};
}

TEST(ListQueryTest, AllowedColumnAscending) {
    EXPECT_EQ(safe_order_by("name", kSortable, "created_at DESC"), "name ASC");
}

TEST(ListQueryTest, AllowedColumnDescendingViaDashPrefix) {
    EXPECT_EQ(safe_order_by("-created_at", kSortable, "id ASC"), "created_at DESC");
}

TEST(ListQueryTest, EmptyFallsBackToDefault) {
    EXPECT_EQ(safe_order_by("", kSortable, "created_at DESC"), "created_at DESC");
}

TEST(ListQueryTest, UnknownColumnIsRejected) {
    // The whole point: a hostile / unknown column never becomes SQL.
    EXPECT_EQ(safe_order_by("password_hash", kSortable, "created_at DESC"), "created_at DESC");
    EXPECT_EQ(safe_order_by("name; DROP TABLE users;--", kSortable, "created_at DESC"), "created_at DESC");
    EXPECT_EQ(safe_order_by("-evil", kSortable, "created_at DESC"), "created_at DESC");
}

TEST(ListQueryTest, BraceListOverload) {
    EXPECT_EQ(safe_order_by("name", {"name", "created_at"}, "id ASC"), "name ASC");
    EXPECT_EQ(safe_order_by("nope", {"name", "created_at"}, "id ASC"), "id ASC");
}
