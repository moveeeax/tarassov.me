#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "core/Core.hpp"
#include "test_helpers.hpp"
#include "utils/Config.hpp"

class ConfigTest : public ::testing::Test {
protected:
    std::string test_config_file = "test_config.json";

    void SetUp() override {
        std::ofstream file(test_config_file);
        file << R"({
            "test": {
                "value": "hello",
                "number": 42,
                "boolean": true
            },
            "nested": {
                "deep": {
                    "value": "world"
                }
            }
        })";
        file.close();
    }

    void TearDown() override {
        if (std::filesystem::exists(test_config_file)) {
            std::filesystem::remove(test_config_file);
        }
        TestHelpers::reset_all_globals();
    }
};

TEST_F(ConfigTest, InitializeAndShutdown) {
    EXPECT_NO_THROW(Config::initialize(test_config_file));
    EXPECT_TRUE(Config::is_initialized());

    EXPECT_NO_THROW(Config::shutdown());
    EXPECT_FALSE(Config::is_initialized());
}

// ── Boot-time prod-safety gate (#32) — refuse auth.mode=none in production ────

TEST_F(ConfigTest, ProdRefusesAuthModeNone) {
    setenv("APP_ENV", "production", 1);
    setenv("AUTH_MODE", "none", 1);
    Config::initialize(test_config_file);
    // The single highest-cost misconfig (every endpoint public in prod) must
    // fail loud, not start quietly insecure.
    EXPECT_THROW(Core::Application::validate_config(Config::get()), std::runtime_error);
    Config::shutdown();
    unsetenv("APP_ENV");
    unsetenv("AUTH_MODE");
}

TEST_F(ConfigTest, ProdAllowsJwt) {
    setenv("APP_ENV", "production", 1);
    setenv("AUTH_MODE", "jwt", 1);
    Config::initialize(test_config_file);
    EXPECT_NO_THROW(Core::Application::validate_config(Config::get()));
    Config::shutdown();
    unsetenv("APP_ENV");
    unsetenv("AUTH_MODE");
}

TEST_F(ConfigTest, DevAllowsAuthModeNone) {
    setenv("APP_ENV", "development", 1);
    setenv("AUTH_MODE", "none", 1);
    Config::initialize(test_config_file);
    EXPECT_NO_THROW(Core::Application::validate_config(Config::get()));
    Config::shutdown();
    unsetenv("APP_ENV");
    unsetenv("AUTH_MODE");
}

TEST_F(ConfigTest, GetStringValue) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    std::string value = config.get<std::string>("test.value", "", "default");
    EXPECT_EQ(value, "hello");
}

TEST_F(ConfigTest, GetIntValue) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    int value = config.get<int>("test.number", "", 0);
    EXPECT_EQ(value, 42);
}

TEST_F(ConfigTest, GetBoolValue) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    bool value = config.get<bool>("test.boolean", "", false);
    EXPECT_TRUE(value);
}

TEST_F(ConfigTest, GetNestedValue) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    std::string value = config.get<std::string>("nested.deep.value", "", "");
    EXPECT_EQ(value, "world");
}

TEST_F(ConfigTest, GetDefaultValue) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    std::string value = config.get<std::string>("nonexistent.key", "", "default_value");
    EXPECT_EQ(value, "default_value");
}

TEST_F(ConfigTest, GetOptionalExisting) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    auto value = config.get_optional<std::string>("test.value");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), "hello");
}

TEST_F(ConfigTest, GetOptionalNonExisting) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    auto value = config.get_optional<std::string>("nonexistent.key");
    EXPECT_FALSE(value.has_value());
}

TEST_F(ConfigTest, DoubleInitializeThrows) {
    Config::initialize(test_config_file);
    EXPECT_THROW(Config::initialize(test_config_file), std::runtime_error);
}

TEST_F(ConfigTest, GetBeforeInitializeThrows) {
    EXPECT_THROW(Config::get(), std::runtime_error);
}

TEST_F(ConfigTest, ReloadConfig) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    EXPECT_EQ(config.get<std::string>("test.value", "", ""), "hello");

    // Modify the file
    std::ofstream file(test_config_file);
    file << R"({"test": {"value": "updated", "number": 99, "boolean": false}})";
    file.close();

    config.reload();
    EXPECT_EQ(config.get<std::string>("test.value", "", ""), "updated");
    EXPECT_EQ(config.get<int>("test.number", "", 0), 99);
}

TEST_F(ConfigTest, EnvironmentVariableOverride) {
    Config::initialize(test_config_file);
    auto& config = Config::get();

    std::string value;
    {
        TestHelpers::ScopedEnv env("TEST_ENV_OVERRIDE", "from_env");
        value = config.get<std::string>("test.value", "TEST_ENV_OVERRIDE", "default");
        EXPECT_EQ(value, "from_env");
    }
    // Without env var, falls back to config
    value = config.get<std::string>("test.value", "TEST_ENV_OVERRIDE", "default");
    EXPECT_EQ(value, "hello");
}

TEST_F(ConfigTest, InvalidJsonFileThrows) {
    auto bad_file = TestHelpers::create_temp_config("{ this is not valid json }}}", "bad_config.json");
    EXPECT_THROW(Config::initialize(bad_file), std::runtime_error);
    TestHelpers::remove_temp_config(bad_file);
}

TEST_F(ConfigTest, NonexistentFileThrows) {
    EXPECT_THROW(Config::initialize("/nonexistent/path/config.json"), std::runtime_error);
}

TEST_F(ConfigTest, EmptyConfigFile) {
    auto empty_file = TestHelpers::create_temp_config("{}", "empty_config.json");
    EXPECT_NO_THROW(Config::initialize(empty_file));
    auto& config = Config::get();
    // Missing key returns default
    EXPECT_EQ(config.get<std::string>("any.key", "", "fallback"), "fallback");
    TestHelpers::remove_temp_config(empty_file);
}

TEST_F(ConfigTest, ShutdownIdempotent) {
    Config::initialize(test_config_file);
    EXPECT_NO_THROW(Config::shutdown());
    EXPECT_NO_THROW(Config::shutdown());
    EXPECT_FALSE(Config::is_initialized());
}

TEST_F(ConfigTest, EnvPlaceholderSubstitutedInStringValue) {
    TestHelpers::ScopedEnv env("CFG_TEST_PWD", "s3cret");
    auto path = TestHelpers::create_temp_config(R"({"db": {"url": "postgres://u:${CFG_TEST_PWD}@h/db"}})",
                                                "placeholder_config.json");
    Config::initialize(path);
    auto& config = Config::get();
    EXPECT_EQ(config.get<std::string>("db.url", "", ""), "postgres://u:s3cret@h/db");
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, EnvPlaceholderWithDefaultFallback) {
    TestHelpers::ScopedEnv env("CFG_TEST_UNSET");  // ensure unset
    auto path =
        TestHelpers::create_temp_config(R"({"v": "${CFG_TEST_UNSET:-fallback_value}"})", "placeholder_default.json");
    Config::initialize(path);
    EXPECT_EQ(Config::get().get<std::string>("v", "", ""), "fallback_value");
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, RequireThrowsWhenMissing) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    EXPECT_THROW((void)config.require<std::string>("no.such.key", "NO_SUCH_ENV_VAR_XYZ"), std::runtime_error);
}

TEST_F(ConfigTest, RequireReturnsEnvOverValue) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    {
        TestHelpers::ScopedEnv env("CFG_REQUIRED", "from_env");
        EXPECT_EQ(config.require<std::string>("test.value", "CFG_REQUIRED"), "from_env");
    }
    // Falls back to config value when env is unset.
    EXPECT_EQ(config.require<std::string>("test.value", "CFG_REQUIRED"), "hello");
}

TEST_F(ConfigTest, GetJsonRawAccess) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    const auto& json_data = config.get_json();
    EXPECT_TRUE(json_data.contains("test"));
    EXPECT_TRUE(json_data.contains("nested"));
    EXPECT_EQ(json_data["test"]["value"], "hello");
}
