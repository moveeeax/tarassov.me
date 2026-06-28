#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "core/Core.hpp"
#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Tests that need a real Postgres + Redis. Config comes from
// TestHelpers::minimal_config() (env-aware hosts, fresh metrics port); the
// suite tests Core::initialize itself, so it writes the config and calls
// initialize per-test rather than deriving from CoreBackedTest.
// ---------------------------------------------------------------------------

class CoreTest : public ::testing::Test {
protected:
    std::string config_path;

    void SetUp() override {
        if (!TestHelpers::is_postgres_available() || !TestHelpers::is_redis_available()) {
            GTEST_SKIP() << "Postgres or Redis not available";
        }
        config_path = TestHelpers::create_temp_config(TestHelpers::minimal_config(), "core_test_config.json");
    }

    void TearDown() override {
        TestHelpers::reset_all_globals();
        if (!config_path.empty() && std::filesystem::exists(config_path)) {
            std::filesystem::remove(config_path);
        }
    }
};

TEST_F(CoreTest, InitializeAllSubsystems) {
    Core::initialize(config_path);
    EXPECT_TRUE(Core::is_initialized());
    EXPECT_TRUE(Config::is_initialized());
    EXPECT_TRUE(Observability::is_initialized());
    EXPECT_TRUE(Database::is_initialized());
    EXPECT_TRUE(Cache::is_initialized());
}

TEST_F(CoreTest, HealthCheckAllGreen) {
    Core::initialize(config_path);
    EXPECT_TRUE(Core::health_check());
}

TEST_F(CoreTest, Shutdown) {
    Core::initialize(config_path);
    EXPECT_TRUE(Core::is_initialized());

    Core::shutdown();
    EXPECT_FALSE(Core::is_initialized());
    EXPECT_FALSE(Database::is_initialized());
    EXPECT_FALSE(Cache::is_initialized());
}

TEST_F(CoreTest, DoubleInitThrows) {
    Core::initialize(config_path);
    EXPECT_THROW(Core::initialize(config_path), std::runtime_error);
}

TEST_F(CoreTest, Version) {
    Core::initialize(config_path);
    // Non-empty is the contract; pinning the literal made every version
    // bump break this test for no signal.
    EXPECT_FALSE(Core::get().version().empty());
}

TEST_F(CoreTest, ReloadConfig) {
    Core::initialize(config_path);
    EXPECT_NO_THROW(Core::get().reload_config());
}

// ---------------------------------------------------------------------------
// Lifecycle guards that need NO services — plain TESTs so they run even in
// environments without the Postgres/Redis sidecars.
// ---------------------------------------------------------------------------

TEST(CoreNoServicesTest, GetBeforeInitThrows) {
    TestHelpers::reset_all_globals();
    EXPECT_THROW(Core::get(), std::runtime_error);
}

TEST(CoreNoServicesTest, InitWithBadConfigThrows) {
    TestHelpers::reset_all_globals();
    EXPECT_THROW(Core::initialize("nonexistent_config.json"), std::runtime_error);
    TestHelpers::reset_all_globals();
}

TEST(CoreNoServicesTest, HealthCheckBeforeInit) {
    TestHelpers::reset_all_globals();
    EXPECT_FALSE(Core::health_check());
}
