/**
 * @file test_account_flow.cpp
 * @brief Integration tests for AccountController.
 *
 * Coverage focuses on the token roundtrip — token issuing happens
 * inside AccountEmails (not directly observable from outside), so
 * tests issue tokens themselves with the same secret + purpose, then
 * call the apply-* endpoint and assert state changes.
 *
 * Skips when Postgres / Redis are not reachable.
 */

#include <chrono>
#include <filesystem>
#include <fstream>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AccountController.hpp"
#include "api/AuthController.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "security/Tokens.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-account-flow";

class AccountFlowTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AuthController auth;
    Api::AccountController account;

    std::string config_file_name() const override { return "account_flow_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["auth"]["cookies"]["enabled"] = true;
        cfg["auth"]["cookies"]["secure"] = false;
        cfg["mail"]["enabled"] = false;  // log-only in tests
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;

        TestHelpers::truncate_users();
    }

    static HttpRequestPtr post_json(const json& body) { return TestHelpers::post_json(body); }

    HttpResponsePtr register_user(const std::string& email, const std::string& password) {
        HttpResponsePtr r;
        auth.registerUser(post_json({{"email", email}, {"password", password}}),
                          [&](const HttpResponsePtr& x) { r = x; });
        return r;
    }
};

TEST_F(AccountFlowTest, confirmTokenMarksUserConfirmed) {
    ASSERT_EQ(register_user("a@example.com", "password1")->statusCode(), k201Created);

    Repositories::UserRepository repo;
    auto user = repo.find_by_email("a@example.com");
    ASSERT_TRUE(user.has_value());
    EXPECT_FALSE(user->confirmed);

    auto token = Security::Tokens::issue(kSecret, user->id, Security::Tokens::Purpose::Confirm, std::chrono::hours(1));

    HttpResponsePtr resp;
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    account.confirm(
        req, [&](const HttpResponsePtr& r) { resp = r; }, token);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);

    auto refreshed = repo.find(user->id);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_TRUE(refreshed->confirmed);
}

TEST_F(AccountFlowTest, confirmRejectsTamperedToken) {
    ASSERT_EQ(register_user("b@example.com", "password1")->statusCode(), k201Created);

    auto token =
        Security::Tokens::issue(kSecret, "some-uuid", Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    // Tamper the FIRST signature character (after the dot): all 6 bits of a
    // non-final base64url char are data, so the flip always changes the
    // decoded bytes. The last char carries discarded padding bits and a
    // fixed-char flip there can be a decode-level no-op.
    const auto sig_start = token.rfind('.') + 1;
    token[sig_start] = (token[sig_start] == 'A') ? 'B' : 'A';

    HttpResponsePtr resp;
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    account.confirm(
        req, [&](const HttpResponsePtr& r) { resp = r; }, token);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_token");
}

TEST_F(AccountFlowTest, resetRequestAlwaysReturns200) {
    // Whether the user exists or not.
    HttpResponsePtr resp;
    account.requestReset(post_json({{"email", "ghost@nowhere.test"}}), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);

    ASSERT_EQ(register_user("c@example.com", "password1")->statusCode(), k201Created);
    HttpResponsePtr resp2;
    account.requestReset(post_json({{"email", "c@example.com"}}), [&](const HttpResponsePtr& r) { resp2 = r; });
    EXPECT_EQ(resp2->statusCode(), k200OK);
}

TEST_F(AccountFlowTest, applyResetRotatesPasswordHash) {
    ASSERT_EQ(register_user("d@example.com", "originalpass")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("d@example.com");
    const std::string old_hash = *user->password_hash;

    auto token =
        Security::Tokens::issue(kSecret, user->id, Security::Tokens::Purpose::ResetPassword, std::chrono::hours(1));
    auto req = post_json({{"new_password", "newpassword42"}});
    HttpResponsePtr resp;
    account.applyReset(
        req, [&](const HttpResponsePtr& r) { resp = r; }, token);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);

    auto refreshed = repo.find(user->id);
    ASSERT_TRUE(refreshed->password_hash.has_value());
    EXPECT_NE(*refreshed->password_hash, old_hash);
    EXPECT_TRUE(Security::Password::verify("newpassword42", *refreshed->password_hash));
    EXPECT_FALSE(Security::Password::verify("originalpass", *refreshed->password_hash));
}

TEST_F(AccountFlowTest, applyResetRefusesShortPassword) {
    ASSERT_EQ(register_user("e@example.com", "originalpass")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("e@example.com");
    auto token =
        Security::Tokens::issue(kSecret, user->id, Security::Tokens::Purpose::ResetPassword, std::chrono::hours(1));
    auto req = post_json({{"new_password", "abc"}});
    HttpResponsePtr resp;
    account.applyReset(
        req, [&](const HttpResponsePtr& r) { resp = r; }, token);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(AccountFlowTest, changeEmailTokenSwapsAddress) {
    ASSERT_EQ(register_user("f@example.com", "password1")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("f@example.com");
    ASSERT_TRUE(user.has_value());

    auto token = Security::Tokens::issue(kSecret,
                                         user->id,
                                         Security::Tokens::Purpose::ChangeEmail,
                                         std::chrono::hours(1),
                                         json{{"new_email", "f-new@example.com"}});
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    HttpResponsePtr resp;
    account.applyChangeEmail(
        req, [&](const HttpResponsePtr& r) { resp = r; }, token);
    ASSERT_EQ(resp->statusCode(), k200OK);

    auto by_new = repo.find_by_email("f-new@example.com");
    ASSERT_TRUE(by_new.has_value());
    EXPECT_EQ(by_new->id, user->id);
    auto by_old = repo.find_by_email("f@example.com");
    EXPECT_FALSE(by_old.has_value());
}

// ── One-shot replay rejection (R3 #19: used_tokens / consume_once) ───────────
// The DB-authoritative one-shot must reject a second submit of the same token.

TEST_F(AccountFlowTest, confirmTokenRejectsReplay) {
    ASSERT_EQ(register_user("replay-confirm@example.com", "password1")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("replay-confirm@example.com");
    auto token = Security::Tokens::issue(kSecret, user->id, Security::Tokens::Purpose::Confirm, std::chrono::hours(1));

    HttpResponsePtr first, second;
    auto req1 = HttpRequest::newHttpRequest();
    req1->setMethod(Post);
    account.confirm(
        req1, [&](const HttpResponsePtr& r) { first = r; }, token);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->statusCode(), k200OK);

    auto req2 = HttpRequest::newHttpRequest();
    req2->setMethod(Post);
    account.confirm(
        req2, [&](const HttpResponsePtr& r) { second = r; }, token);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->statusCode(), k400BadRequest);  // one-shot: token already used
}

TEST_F(AccountFlowTest, applyResetRejectsReplay) {
    ASSERT_EQ(register_user("replay-reset@example.com", "originalpass")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("replay-reset@example.com");
    auto token =
        Security::Tokens::issue(kSecret, user->id, Security::Tokens::Purpose::ResetPassword, std::chrono::hours(1));

    HttpResponsePtr first, second;
    account.applyReset(
        post_json({{"new_password", "newpassword42"}}), [&](const HttpResponsePtr& r) { first = r; }, token);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->statusCode(), k200OK);

    account.applyReset(
        post_json({{"new_password", "thirdpassword99"}}), [&](const HttpResponsePtr& r) { second = r; }, token);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->statusCode(), k400BadRequest);
    // The replay must not have overwritten the password set by the first use.
    auto refreshed = repo.find(user->id);
    ASSERT_TRUE(refreshed->password_hash.has_value());
    EXPECT_TRUE(Security::Password::verify("newpassword42", *refreshed->password_hash));
}

TEST_F(AccountFlowTest, applyChangeEmailRejectsReplay) {
    ASSERT_EQ(register_user("replay-ce@example.com", "password1")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("replay-ce@example.com");
    auto token = Security::Tokens::issue(kSecret,
                                         user->id,
                                         Security::Tokens::Purpose::ChangeEmail,
                                         std::chrono::hours(1),
                                         json{{"new_email", "replay-ce-new@example.com"}});

    HttpResponsePtr first, second;
    auto req1 = HttpRequest::newHttpRequest();
    req1->setMethod(Post);
    account.applyChangeEmail(
        req1, [&](const HttpResponsePtr& r) { first = r; }, token);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->statusCode(), k200OK);

    auto req2 = HttpRequest::newHttpRequest();
    req2->setMethod(Post);
    account.applyChangeEmail(
        req2, [&](const HttpResponsePtr& r) { second = r; }, token);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->statusCode(), k400BadRequest);  // one-shot (same-user precheck passes, nonce blocks)
}

TEST_F(AccountFlowTest, changeEmailConflictsOnDuplicate) {
    ASSERT_EQ(register_user("g@example.com", "password1")->statusCode(), k201Created);
    ASSERT_EQ(register_user("h@example.com", "password1")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("g@example.com");
    ASSERT_TRUE(user.has_value());

    auto token = Security::Tokens::issue(kSecret,
                                         user->id,
                                         Security::Tokens::Purpose::ChangeEmail,
                                         std::chrono::hours(1),
                                         json{{"new_email", "h@example.com"}});  // already taken
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Post);
    HttpResponsePtr resp;
    account.applyChangeEmail(
        req, [&](const HttpResponsePtr& r) { resp = r; }, token);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
}

TEST_F(AccountFlowTest, changePasswordRequiresOldPassword) {
    ASSERT_EQ(register_user("i@example.com", "originalpass")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("i@example.com");

    // Build req with synthesized principal.
    auto req = post_json({{"old_password", "WRONG"}, {"new_password", "newpassword42"}});
    Security::Auth::AuthPrincipal p;
    p.subject = user->id;
    req->attributes()->insert(Security::Auth::kPrincipalAttr, p);
    HttpResponsePtr resp;
    account.changePassword(req, [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k401Unauthorized);

    // Correct old password succeeds.
    auto good = post_json({{"old_password", "originalpass"}, {"new_password", "newpassword42"}});
    good->attributes()->insert(Security::Auth::kPrincipalAttr, p);
    HttpResponsePtr good_resp;
    account.changePassword(good, [&](const HttpResponsePtr& r) { good_resp = r; });
    EXPECT_EQ(good_resp->statusCode(), k200OK);

    auto refreshed = repo.find(user->id);
    EXPECT_TRUE(Security::Password::verify("newpassword42", *refreshed->password_hash));
}

// ── Invite redeem (POST /api/account/join-from-invite/{token}) ──────────────

namespace {
// Create a pending invite: a user with no password, unconfirmed — exactly what
// AdminController::inviteUser produces.
Domain::User make_invited(const std::string& email) {
    Repositories::RoleRepository roles;
    auto role = roles.find_default();
    EXPECT_TRUE(role.has_value());
    Repositories::UserRepository repo;
    return repo.create(email, std::nullopt, std::nullopt, std::nullopt, role->id, /*confirmed=*/false);
}
std::string invite_token(const std::string& uid) {
    return Security::Tokens::issue(kSecret, uid, Security::Tokens::Purpose::Invite, std::chrono::hours(24 * 7));
}
}  // namespace

TEST_F(AccountFlowTest, joinFromInviteSetsPasswordAndConfirms) {
    auto invited = make_invited("invitee@example.com");
    EXPECT_FALSE(invited.confirmed);

    HttpResponsePtr resp;
    account.joinFromInvite(
        post_json({{"new_password", "newpassword1"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        invite_token(invited.id));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);

    Repositories::UserRepository repo;
    auto fresh = repo.find(invited.id, /*from_primary=*/true);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_TRUE(fresh->confirmed);
    ASSERT_TRUE(fresh->password_hash.has_value());
    EXPECT_TRUE(Security::Password::verify("newpassword1", *fresh->password_hash));
}

// Security regression guard: a validly-signed invite token must NOT be usable to
// reset the password of an already-active (confirmed / password-set) account.
TEST_F(AccountFlowTest, joinFromInviteRefusesActiveAccount) {
    ASSERT_EQ(register_user("active@example.com", "password1")->statusCode(), k201Created);
    Repositories::UserRepository repo;
    auto user = repo.find_by_email("active@example.com");
    ASSERT_TRUE(user.has_value());

    HttpResponsePtr resp;
    account.joinFromInvite(
        post_json({{"new_password", "attacker-pw1"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        invite_token(user->id));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    // Password must be unchanged.
    auto fresh = repo.find(user->id, /*from_primary=*/true);
    EXPECT_TRUE(Security::Password::verify("password1", *fresh->password_hash));
    EXPECT_FALSE(Security::Password::verify("attacker-pw1", *fresh->password_hash));
}

TEST_F(AccountFlowTest, joinFromInviteReplayRejected) {
    auto invited = make_invited("replay@example.com");
    const auto token = invite_token(invited.id);

    HttpResponsePtr first;
    account.joinFromInvite(
        post_json({{"new_password", "firstpass1"}}), [&](const HttpResponsePtr& r) { first = r; }, token);
    EXPECT_EQ(first->statusCode(), k200OK);

    // Second use of the same token is a no-op at the DB layer (account is now
    // confirmed + has a password) → rejected, password not overwritten.
    HttpResponsePtr second;
    account.joinFromInvite(
        post_json({{"new_password", "secondpass1"}}), [&](const HttpResponsePtr& r) { second = r; }, token);
    EXPECT_EQ(second->statusCode(), k400BadRequest);

    Repositories::UserRepository repo;
    auto fresh = repo.find(invited.id, /*from_primary=*/true);
    EXPECT_TRUE(Security::Password::verify("firstpass1", *fresh->password_hash));
}

TEST_F(AccountFlowTest, joinFromInviteRejectsWrongPurposeToken) {
    auto invited = make_invited("wrongpurpose@example.com");
    // A Confirm token must not satisfy the Invite-purpose check.
    auto confirm_token =
        Security::Tokens::issue(kSecret, invited.id, Security::Tokens::Purpose::Confirm, std::chrono::hours(1));
    HttpResponsePtr resp;
    account.joinFromInvite(
        post_json({{"new_password", "whatever12"}}), [&](const HttpResponsePtr& r) { resp = r; }, confirm_token);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

}  // namespace
