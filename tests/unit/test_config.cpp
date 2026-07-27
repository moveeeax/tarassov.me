#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "core/Core.hpp"
#include "test_helpers.hpp"
#include "utils/Config.hpp"

namespace {

/**
 * @brief Redirect the default spdlog logger into a string for the scope, then
 *        hand logging back to the harness's fallback logger.
 * @details AppConfig::get() reports a present-but-unusable key with
 *          spdlog::error and then returns the caller's default. That log line
 *          is the ONLY externally visible difference between "key absent" and
 *          "key present but garbage" — both return the default, and
 *          get_optional/require collapse them too — so asserting on it is the
 *          only way to pin the M3 contract that a typo is not silently
 *          indistinguishable from an unset key.
 */
class CapturedLog {
public:
    CapturedLog() {
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_, /*force_flush=*/true);
        auto logger = std::make_shared<spdlog::logger>(kLoggerName, sink);
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(logger);
    }

    /// Restore the harness logger, then DROP ours from spdlog's registry:
    /// set_default_logger also registers by name, and a leftover entry would
    /// outlive stream_ and hold a dangling std::ostream& for any later flush.
    ~CapturedLog() {
        TestHelpers::restore_default_spdlog();
        spdlog::drop(kLoggerName);
    }

    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    std::string text() const { return stream_.str(); }

private:
    static constexpr const char* kLoggerName = "config_test_capture";

    std::ostringstream stream_;
};

}  // namespace

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
    // Prod also requires a canonical https site.base_url (SEO URL generation).
    setenv("SITE_BASE_URL", "https://example.test", 1);
    Config::initialize(test_config_file);
    EXPECT_NO_THROW(Core::Application::validate_config(Config::get()));
    Config::shutdown();
    unsetenv("APP_ENV");
    unsetenv("AUTH_MODE");
    unsetenv("SITE_BASE_URL");
}

TEST_F(ConfigTest, ProdRequiresHttpsSiteBaseUrl) {
    setenv("APP_ENV", "production", 1);
    setenv("AUTH_MODE", "jwt", 1);
    // Missing → refuse to start.
    Config::initialize(test_config_file);
    EXPECT_THROW(Core::Application::validate_config(Config::get()), std::runtime_error);
    Config::shutdown();
    // http:// (not https) → refuse to start.
    setenv("SITE_BASE_URL", "http://example.test", 1);
    Config::initialize(test_config_file);
    EXPECT_THROW(Core::Application::validate_config(Config::get()), std::runtime_error);
    Config::shutdown();
    unsetenv("APP_ENV");
    unsetenv("AUTH_MODE");
    unsetenv("SITE_BASE_URL");
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

// ── M3: string-shaped leaves coerce to the requested type ───────────────────
// substitute_env_placeholders writes every ${VAR:-default} expansion back as a
// JSON *string*, so a typed key declared as "enabled": "${MAIL_ENABLED:-true}"
// reaches get<bool>() as the string "true". Before the fix nlohmann's
// type_error was swallowed by a blanket catch(...) and the CALLER's C++ default
// silently won — the file's own declared default never applied to any
// placeholder-shaped key.

TEST_F(ConfigTest, StringLeavesCoerceToTypedValues) {
    const std::string doc = R"({"flag": "true", "off": "false", "num": "587", "big": "123456789", "ratio": "1.5"})";
    auto path = TestHelpers::create_temp_config(doc, "coerce_config.json");
    Config::initialize(path);
    auto& config = Config::get();
    EXPECT_TRUE(config.get<bool>("flag", "", false));
    EXPECT_FALSE(config.get<bool>("off", "", true));
    EXPECT_EQ(config.get<int>("num", "", 0), 587);
    EXPECT_EQ(config.get<long>("big", "", 0L), 123456789L);
    EXPECT_DOUBLE_EQ(config.get<double>("ratio", "", 0.0), 1.5);
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, StringLeavesCoerceThroughAnEnvPlaceholderDefault) {
    // The shape this fix exists for: a typed key written as a placeholder with
    // an inline default, with the variable unset.
    TestHelpers::ScopedEnv unset_flag("CFG_TEST_FEATURE");
    TestHelpers::ScopedEnv unset_port("CFG_TEST_PORT");
    const std::string doc = R"({"mail": {"enabled": "${CFG_TEST_FEATURE:-true}", "port": "${CFG_TEST_PORT:-2525}"}})";
    auto path = TestHelpers::create_temp_config(doc, "coerce_placeholder_config.json");
    Config::initialize(path);
    auto& config = Config::get();
    // The FILE's declared default wins — not the caller's C++ fallback.
    EXPECT_TRUE(config.get<bool>("mail.enabled", "", false));
    EXPECT_EQ(config.get<int>("mail.port", "", 25), 2525);
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, StringLeafBooleansFollowEnvVarSpelling) {
    // Coercion routes through parse_env_value, so a string leaf is parsed
    // EXACTLY like the equivalent environment override: "true"/"1"/"yes" are
    // true and — importantly — everything else is false rather than falling
    // back to the caller's default. Pinned so a future case-fold or a
    // throw-on-garbage is a deliberate change, not a surprise.
    const std::string doc = R"({"a": "1", "b": "yes", "c": "TRUE", "d": "nope", "e": "0"})";
    auto path = TestHelpers::create_temp_config(doc, "coerce_bool_config.json");
    Config::initialize(path);
    auto& config = Config::get();
    EXPECT_TRUE(config.get<bool>("a", "", false));
    EXPECT_TRUE(config.get<bool>("b", "", false));
    EXPECT_FALSE(config.get<bool>("c", "", true)) << "bool spelling is case-sensitive";
    EXPECT_FALSE(config.get<bool>("d", "", true));
    EXPECT_FALSE(config.get<bool>("e", "", true));
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, NativelyTypedLeavesStillWin) {
    // Coercion must not regress the ordinary case: a real JSON number/bool is
    // read straight through, no string round-trip.
    Config::initialize(test_config_file);
    auto& config = Config::get();
    EXPECT_EQ(config.get<int>("test.number", "", 0), 42);
    EXPECT_TRUE(config.get<bool>("test.boolean", "", false));
    EXPECT_EQ(config.get<std::string>("test.value", "", ""), "hello");
}

TEST_F(ConfigTest, EmptyPlaceholderExpansionTakesTheDefault) {
    // "${VAR}" with VAR unset expands to "" — that means "not set", not 0 or
    // false, so a typed read takes the caller's default instead of logging.
    TestHelpers::ScopedEnv unset("CFG_TEST_EMPTY");
    const std::string doc = R"({"db": {"port": "${CFG_TEST_EMPTY}", "ssl": "${CFG_TEST_EMPTY}"}})";
    auto path = TestHelpers::create_temp_config(doc, "empty_placeholder_config.json");
    Config::initialize(path);
    auto& config = Config::get();
    EXPECT_EQ(config.get<int>("db.port", "", 5432), 5432);
    EXPECT_TRUE(config.get<bool>("db.ssl", "", true));
    // A string read keeps the empty string: "" is a legitimate string value.
    EXPECT_EQ(config.get<std::string>("db.port", "", "fallback"), "");
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, AbsentKeyStillReturnsTheTypedDefault) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    EXPECT_EQ(config.get<int>("test.no_such_number", "", 587), 587);
    EXPECT_TRUE(config.get<bool>("test.no_such_flag", "", true));
    EXPECT_EQ(config.get<std::string>("nested.deep.no_such_value", "", "fallback"), "fallback");
    // A path that walks THROUGH a scalar is absent too, not an exception.
    EXPECT_EQ(config.get<int>("test.number.deeper", "", 42), 42);
}

TEST_F(ConfigTest, AbsentKeyIsNotLogged) {
    // The mirror image of the test below: an unset key is a normal condition
    // and must stay silent, or the error log is noise at every boot.
    Config::initialize(test_config_file);
    auto& config = Config::get();
    std::string logged;
    {
        CapturedLog captured;
        EXPECT_EQ(config.get<int>("test.no_such_number", "", 587), 587);
        logged = captured.text();
    }
    EXPECT_EQ(logged.find("test.no_such_number"), std::string::npos) << logged;
}

TEST_F(ConfigTest, UnusableValueLogsAnErrorBeforeFallingBackToTheDefault) {
    const std::string doc = R"({"num": "not-a-number", "obj": {"nested": 1}})";
    auto path = TestHelpers::create_temp_config(doc, "garbage_config.json");
    Config::initialize(path);
    auto& config = Config::get();

    std::string logged;
    int num = 0;
    int obj = 0;
    {
        CapturedLog captured;
        num = config.get<int>("num", "", 587);
        obj = config.get<int>("obj", "", 99);
        logged = captured.text();
    }
    // Behaviour: the default is still returned — a typo must not crash boot.
    EXPECT_EQ(num, 587);
    EXPECT_EQ(obj, 99);
    // Contract: but it is NOT silent. A garbage value used to be
    // indistinguishable from a deliberately unset key.
    EXPECT_NE(logged.find("Config: key 'num'"), std::string::npos) << logged;
    EXPECT_NE(logged.find("Config: key 'obj'"), std::string::npos) << logged;
    EXPECT_NE(logged.find("using default"), std::string::npos) << logged;
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, UnusableValueIsAbsentForOptionalAndFatalForRequire) {
    auto path = TestHelpers::create_temp_config(R"({"num": "not-a-number"})", "garbage_strict_config.json");
    Config::initialize(path);
    auto& config = Config::get();
    EXPECT_FALSE(config.get_optional<int>("num").has_value());
    EXPECT_THROW((void)config.require<int>("num", "NO_SUCH_ENV_VAR_XYZ"), std::runtime_error);
    // The same leaf read as a string is perfectly usable — only the typed
    // conversion fails.
    EXPECT_EQ(config.get<std::string>("num", "", ""), "not-a-number");
    TestHelpers::remove_temp_config(path);
}

TEST_F(ConfigTest, GetJsonRawAccess) {
    Config::initialize(test_config_file);
    auto& config = Config::get();
    const auto& json_data = config.get_json();
    EXPECT_TRUE(json_data.contains("test"));
    EXPECT_TRUE(json_data.contains("nested"));
    EXPECT_EQ(json_data["test"]["value"], "hello");
}
