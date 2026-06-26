/**
 * @file test_migration_marker.cpp
 * @brief Unit tests for the `-- migrate:no-transaction` marker detection that
 *        routes a migration to the autocommit (CONCURRENTLY-capable) path.
 *        Pure string logic — no database needed.
 */

#include <gtest/gtest.h>

#include "database/Migrations.hpp"

TEST(MigrationMarkerTest, DetectsMarkerOnFirstLine) {
    EXPECT_TRUE(Migrations::has_no_transaction_marker(
        "-- migrate:no-transaction\nCREATE INDEX CONCURRENTLY idx ON t (col);\n"));
}

TEST(MigrationMarkerTest, DetectsMarkerAnywhere) {
    EXPECT_TRUE(
        Migrations::has_no_transaction_marker("CREATE INDEX CONCURRENTLY idx ON t (col);\n"
                                              "-- migrate:no-transaction\n"));
}

TEST(MigrationMarkerTest, PlainMigrationHasNoMarker) {
    EXPECT_FALSE(Migrations::has_no_transaction_marker("CREATE TABLE t (id int primary key);"));
}

TEST(MigrationMarkerTest, UnrelatedCommentIsNotTheMarker) {
    EXPECT_FALSE(Migrations::has_no_transaction_marker("-- add a column\nALTER TABLE t ADD COLUMN c int;"));
}
