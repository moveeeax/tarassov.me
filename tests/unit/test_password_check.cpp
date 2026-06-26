#include <cstdlib>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "core/Core.hpp"
#include "test_helpers.hpp"

// --- "passes silently" path -------------------------------------------------

TEST(PasswordSafetyCheck, AcceptsStrongPassword) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    EXPECT_NO_THROW(Core::check_password_safety("postgresql://user:s3cret-AAA-bbb@db:5432/app"));
}

TEST(PasswordSafetyCheck, IgnoresKeyValueForm) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    // libpq key=value form has no userinfo to inspect — skipped entirely.
    EXPECT_NO_THROW(Core::check_password_safety("host=db user=postgres password=postgres dbname=app"));
}

TEST(PasswordSafetyCheck, IgnoresUrlWithoutUserinfo) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    EXPECT_NO_THROW(Core::check_password_safety("postgresql://db:5432/app"));
}

// --- "warns by default" path: returns normally even with weak password ------

TEST(PasswordSafetyCheck, WarnsButDoesNotThrowByDefault) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD");  // unset
    EXPECT_NO_THROW(Core::check_password_safety("postgresql://postgres:postgres@db:5432/app"));
}

TEST(PasswordSafetyCheck, WarnsButDoesNotThrowWhenFlagFalse) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "false");
    EXPECT_NO_THROW(Core::check_password_safety("postgresql://postgres:postgres@db:5432/app"));
}

// --- "throws when enforced" path -------------------------------------------

TEST(PasswordSafetyCheck, ThrowsOnDefaultPassword) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    EXPECT_THROW(Core::check_password_safety("postgresql://postgres:postgres@db:5432/app"), std::runtime_error);
}

TEST(PasswordSafetyCheck, ThrowsOnEmptyPassword) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    EXPECT_THROW(Core::check_password_safety("postgresql://postgres:@db:5432/app"), std::runtime_error);
}

TEST(PasswordSafetyCheck, ThrowsOnPasswordLiteral) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    EXPECT_THROW(Core::check_password_safety("postgresql://app:password@db:5432/app"), std::runtime_error);
}

TEST(PasswordSafetyCheck, ThrowsOnChangeme) {
    TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "true");
    EXPECT_THROW(Core::check_password_safety("postgresql://app:changeme@db:5432/app"), std::runtime_error);
}

TEST(PasswordSafetyCheck, EnforceFlagAccepts1AndYes) {
    {
        TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "1");
        EXPECT_THROW(Core::check_password_safety("postgresql://postgres:postgres@db/app"), std::runtime_error);
    }
    {
        TestHelpers::ScopedEnv _flag("DATABASE_REQUIRE_SECURE_PASSWORD", "yes");
        EXPECT_THROW(Core::check_password_safety("postgresql://postgres:postgres@db/app"), std::runtime_error);
    }
}
