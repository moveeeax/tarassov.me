/**
 * @file test_tokens.cpp
 * @brief Unit tests for Security::Tokens (HMAC-signed timed link tokens).
 *
 * Pure crypto — no Postgres / Redis.
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "security/Tokens.hpp"

namespace {

constexpr const char* kSecret = "test-secret-very-long-and-random";

}  // namespace

TEST(TokensTest, issueAndVerifyRoundtrip) {
    auto t = Security::Tokens::issue(kSecret, "user-123", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    auto vr = Security::Tokens::verify(kSecret, t, Security::Tokens::Purpose::Confirm);
    EXPECT_TRUE(vr.ok) << "error: " << vr.error;
    EXPECT_EQ(vr.sub, "user-123");
    EXPECT_TRUE(vr.error.empty());
}

TEST(TokensTest, rejectsTamperedPayload) {
    auto t = Security::Tokens::issue(kSecret, "user-123", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    // Flip one character in the payload. Signature still references the
    // original; verification should fail with invalid_signature.
    auto dot = t.find('.');
    ASSERT_NE(dot, std::string::npos);
    t[0] = (t[0] == 'A' ? 'B' : 'A');
    auto vr = Security::Tokens::verify(kSecret, t, Security::Tokens::Purpose::Confirm);
    EXPECT_FALSE(vr.ok);
}

TEST(TokensTest, rejectsTamperedSignature) {
    auto t = Security::Tokens::issue(kSecret, "user-123", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    auto dot = t.find('.');
    ASSERT_NE(dot, std::string::npos);
    // Tamper the FIRST signature character, not the last: the final char of
    // unpadded base64url carries only 4 data bits + 2 padding bits that the
    // decoder discards, so an 'A'<->'B' flip there decodes to the very same
    // bytes ~1/16 of the time and the signature legitimately verifies.
    t[dot + 1] = (t[dot + 1] == 'A' ? 'B' : 'A');
    auto vr = Security::Tokens::verify(kSecret, t, Security::Tokens::Purpose::Confirm);
    EXPECT_FALSE(vr.ok);
}

TEST(TokensTest, rejectsMalleatedSignature) {
    // Token malleability: a *different* token string whose signature segment
    // decodes to the same HMAC bytes used to slip past the constant-time
    // compare (and any one-shot/JTI replay guard keyed on the raw string).
    // Base64::url_decode now rejects non-canonical encodings, so appending a
    // char to the signature — or flipping a trailing char so its padding bits
    // become non-zero — must fail verification rather than re-validate.
    auto t = Security::Tokens::issue(kSecret, "user-123", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    auto vr = Security::Tokens::verify(kSecret, t + "A", Security::Tokens::Purpose::Confirm);
    EXPECT_FALSE(vr.ok);
}

TEST(TokensTest, rejectsWrongPurpose) {
    auto t = Security::Tokens::issue(kSecret, "user-123", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    auto vr = Security::Tokens::verify(kSecret, t, Security::Tokens::Purpose::ResetPassword);
    EXPECT_FALSE(vr.ok);
    // Wrong purpose surfaces either as wrong_purpose (after signature
    // verification) or invalid_signature (per-purpose key derivation
    // means a confirm-key signature won't validate against reset-key).
    // Either is acceptable; we only assert "not ok".
}

TEST(TokensTest, rejectsExpired) {
    // Negative TTL mints an already-expired token — deterministic, unlike
    // the old ttl=1s + sleep(1.5s) variant, which raced integer-second
    // epoch arithmetic and failed whenever issue landed in the first half
    // of a wall-clock second.
    auto t = Security::Tokens::issue(
        kSecret, "user-123", Security::Tokens::Purpose::ResetPassword, std::chrono::seconds(-2));
    auto vr = Security::Tokens::verify(kSecret, t, Security::Tokens::Purpose::ResetPassword);
    EXPECT_FALSE(vr.ok);
    EXPECT_EQ(vr.error, "expired");
}

TEST(TokensTest, rejectsMalformed) {
    EXPECT_FALSE(Security::Tokens::verify(kSecret, "", Security::Tokens::Purpose::Confirm).ok);
    EXPECT_FALSE(Security::Tokens::verify(kSecret, "no-dot-here", Security::Tokens::Purpose::Confirm).ok);
    EXPECT_FALSE(Security::Tokens::verify(kSecret, "bad..token", Security::Tokens::Purpose::Confirm).ok);
}

TEST(TokensTest, perPurposeKeyDerivationIsolatesPurposes) {
    // Same secret + same payload → different signature for different purposes.
    auto a = Security::Tokens::issue(kSecret, "u", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    auto b = Security::Tokens::issue(kSecret, "u", Security::Tokens::Purpose::ResetPassword, std::chrono::hours(1));
    // Tokens differ because purpose appears in payload AND in key derivation.
    EXPECT_NE(a, b);
}

TEST(TokensTest, carriesExtraClaims) {
    nlohmann::json extra = {{"new_email", "alice@example.com"}};
    auto t =
        Security::Tokens::issue(kSecret, "u-1", Security::Tokens::Purpose::ChangeEmail, std::chrono::hours(1), extra);
    auto vr = Security::Tokens::verify(kSecret, t, Security::Tokens::Purpose::ChangeEmail);
    ASSERT_TRUE(vr.ok);
    ASSERT_TRUE(vr.extra.contains("new_email"));
    EXPECT_EQ(vr.extra["new_email"].get<std::string>(), "alice@example.com");
}

TEST(TokensTest, refusesEmptySecret) {
    EXPECT_THROW(Security::Tokens::issue("", "u", Security::Tokens::Purpose::Confirm, std::chrono::hours(1)),
                 std::invalid_argument);
}
