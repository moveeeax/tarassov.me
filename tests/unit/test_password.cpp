/**
 * @file test_password.cpp
 * @brief Unit tests for Security::Password (argon2id via libsodium).
 *
 * Pure crypto — no Postgres / Redis. Should run in <100ms per case
 * thanks to the INTERACTIVE preset.
 */

#include <gtest/gtest.h>

#include "security/Password.hpp"

TEST(PasswordTest, HashesAreNonEmptyAndArgon2id) {
    auto hash = Security::Password::hash("correct horse battery staple");
    EXPECT_FALSE(hash.empty());
    EXPECT_TRUE(Security::Password::looks_hashed(hash));
    EXPECT_TRUE(hash.starts_with("$argon2"));
}

TEST(PasswordTest, HashIsRandom) {
    // Two hashes of the same password differ — argon2 includes random salt.
    auto h1 = Security::Password::hash("hunter2");
    auto h2 = Security::Password::hash("hunter2");
    EXPECT_NE(h1, h2);
    EXPECT_TRUE(Security::Password::verify("hunter2", h1));
    EXPECT_TRUE(Security::Password::verify("hunter2", h2));
}

TEST(PasswordTest, VerifyAcceptsCorrectPassword) {
    auto h = Security::Password::hash("password123");
    EXPECT_TRUE(Security::Password::verify("password123", h));
}

TEST(PasswordTest, VerifyRejectsWrongPassword) {
    auto h = Security::Password::hash("password123");
    EXPECT_FALSE(Security::Password::verify("password124", h));
    EXPECT_FALSE(Security::Password::verify("", h));
    EXPECT_FALSE(Security::Password::verify("password1234", h));
}

TEST(PasswordTest, VerifyHandlesEmptyAndMalformed) {
    EXPECT_FALSE(Security::Password::verify("", ""));
    EXPECT_FALSE(Security::Password::verify("x", ""));
    EXPECT_FALSE(Security::Password::verify("x", "$not-an-argon2-hash$"));
    // Truncated hash — must not crash.
    auto h = Security::Password::hash("a");
    EXPECT_FALSE(Security::Password::verify("a", h.substr(0, h.size() / 2)));
}

TEST(PasswordTest, LooksHashedRecognizesArgon2Variants) {
    EXPECT_TRUE(Security::Password::looks_hashed("$argon2id$v=19$m=65536,t=2,p=1$AAAA$BBBB"));
    EXPECT_TRUE(Security::Password::looks_hashed("$argon2i$v=19$..."));
    EXPECT_TRUE(Security::Password::looks_hashed("$argon2d$v=19$..."));
    EXPECT_FALSE(Security::Password::looks_hashed("$bcrypt$..."));
    EXPECT_FALSE(Security::Password::looks_hashed("plaintext"));
    EXPECT_FALSE(Security::Password::looks_hashed(""));
}

TEST(PasswordTest, HashRefusesEmptyPassword) {
    EXPECT_THROW(Security::Password::hash(""), std::invalid_argument);
}
