/**
 * @file test_migrations.cpp
 * @brief Integration tests for the MigrationRunner (needs real Postgres).
 *
 * Covers the behaviours that silently corrupt schema state if broken:
 * apply-pending, idempotent re-run (skip already-applied — the loser of a
 * concurrent boot relies on this), and the read-only list_pending tracker.
 * Uses a private temp migrations dir + a private tracking table-free schema
 * so it doesn't collide with the real 001_users_and_roles migration.
 */

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "database/Migrations.hpp"
#include "test_helpers.hpp"

namespace fs = std::filesystem;

namespace {

class MigrationsTest : public ::testing::Test {
protected:
    fs::path dir_;

    void SetUp() override {
        if (!TestHelpers::is_postgres_available())
            GTEST_SKIP() << "PostgreSQL not available";
        TestHelpers::reset_all_globals();
        Database::initialize(TestHelpers::pg_conn_string(), {}, 2);

        // Clean slate: drop the test artifacts a previous run may have left.
        Database::get().execute_write([](auto& txn) {
            txn.exec("DROP TABLE IF EXISTS mig_test_widget");
            txn.exec("DROP TABLE IF EXISTS schema_migrations");
            return 0;
        });

        dir_ = fs::temp_directory_path() / "mig_test";
        fs::create_directories(dir_);
        std::ofstream(dir_ / "001_widget.sql")
            << "CREATE TABLE IF NOT EXISTS mig_test_widget (id serial primary key);\n";
    }

    void TearDown() override {
        if (Database::is_initialized()) {
            try {
                Database::get().execute_write([](auto& txn) {
                    txn.exec("DROP TABLE IF EXISTS mig_test_widget");
                    txn.exec("DROP TABLE IF EXISTS schema_migrations");
                    return 0;
                });
            } catch (...) {}
        }
        TestHelpers::reset_all_globals();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    long applied_count() {
        return Database::get().execute_read(
            [](auto& txn) { return txn.exec("SELECT COUNT(*) FROM schema_migrations")[0][0].template as<long>(); });
    }
};

TEST_F(MigrationsTest, AppliesPendingAndTracks) {
    EXPECT_EQ(Migrations::MigrationRunner::list_pending(dir_.string()).size(), 1u);

    Migrations::MigrationRunner runner;
    runner.initialize(dir_.string());

    EXPECT_EQ(applied_count(), 1);
    EXPECT_TRUE(Migrations::MigrationRunner::list_pending(dir_.string()).empty());
    // The migration actually ran.
    auto exists = Database::get().execute_read([](auto& txn) {
        return txn.exec("SELECT to_regclass('public.mig_test_widget') IS NOT NULL")[0][0].template as<bool>();
    });
    EXPECT_TRUE(exists);
}

TEST_F(MigrationsTest, ReRunIsIdempotent) {
    Migrations::MigrationRunner().initialize(dir_.string());
    ASSERT_EQ(applied_count(), 1);

    // A second runner over the same dir must skip the applied migration, not
    // re-run the DDL or duplicate the tracking row.
    Migrations::MigrationRunner runner2;
    EXPECT_NO_THROW(runner2.initialize(dir_.string()));
    EXPECT_EQ(applied_count(), 1);
}

TEST_F(MigrationsTest, NewMigrationAppliedOnNextRun) {
    Migrations::MigrationRunner().initialize(dir_.string());
    ASSERT_EQ(applied_count(), 1);

    std::ofstream(dir_ / "002_widget_col.sql") << "ALTER TABLE mig_test_widget ADD COLUMN IF NOT EXISTS label text;\n";
    EXPECT_EQ(Migrations::MigrationRunner::list_pending(dir_.string()).size(), 1u);

    Migrations::MigrationRunner().initialize(dir_.string());
    EXPECT_EQ(applied_count(), 2);
    EXPECT_TRUE(Migrations::MigrationRunner::list_pending(dir_.string()).empty());
}

}  // namespace
