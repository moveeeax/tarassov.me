/**
 * @file test_csrf.cpp
 * @brief Unit tests for the double-submit CSRF decision (Security::Csrf::passes).
 */

#include <gtest/gtest.h>

#include "security/Csrf.hpp"

namespace Csrf = Security::Csrf;

// Safe methods never mutate state — always allowed, token irrelevant.
TEST(CsrfPasses, SafeMethodAlwaysAllowed) {
    EXPECT_TRUE(Csrf::passes(/*unsafe=*/false, "access", "", ""));
    EXPECT_TRUE(Csrf::passes(/*unsafe=*/false, "access", "tok", "mismatch"));
}

// No access cookie → Bearer/unauthenticated → not CSRF-able, skip the check.
TEST(CsrfPasses, NonCookieAuthSkipped) {
    EXPECT_TRUE(Csrf::passes(/*unsafe=*/true, "", "", ""));
    EXPECT_TRUE(Csrf::passes(/*unsafe=*/true, "", "tok", "different"));
}

// Cookie-auth mutation with matching token passes.
TEST(CsrfPasses, CookieAuthMatchingTokenPasses) {
    EXPECT_TRUE(Csrf::passes(/*unsafe=*/true, "access", "tok123", "tok123"));
}

// Cookie-auth mutation is rejected when the token is missing or mismatched.
TEST(CsrfPasses, CookieAuthMissingOrMismatchedRejected) {
    EXPECT_FALSE(Csrf::passes(/*unsafe=*/true, "access", "tok", "other"));
    EXPECT_FALSE(Csrf::passes(/*unsafe=*/true, "access", "", "tok"));
    EXPECT_FALSE(Csrf::passes(/*unsafe=*/true, "access", "tok", ""));
    EXPECT_FALSE(Csrf::passes(/*unsafe=*/true, "access", "", ""));
}
