/**
 * @file test_contact.cpp
 * @brief Integration tests for ContactController (POST /api/v1/public/contact).
 *        Validates the 400/503 paths and CRLF (header-injection) rejection.
 *        The actual SMTP send is fire-and-forget (best-effort) so a configured
 *        recipient yields 200 even with the mailer disabled.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/ContactController.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

template <typename Fn>
HttpResponsePtr run(Fn&& fn) {
    HttpResponsePtr resp;
    fn([&](const HttpResponsePtr& r) { resp = r; });
    return resp;
}

// Recipient configured → submissions are accepted.
class ContactConfiguredTest : public TestHelpers::CoreBackedTest {
protected:
    Api::ContactController controller;
    std::string config_file_name() const override { return "contact_configured_test_config.json"; }
    void config_overrides(json& cfg) override { cfg["mail"]["contact_to"] = "owner@example.com"; }
};

TEST_F(ContactConfiguredTest, ValidSubmissionAccepted) {
    json body = {{"name", "Ann"}, {"email", "ann@example.com"}, {"subject", "Hi"}, {"message", "hello there"}};
    auto resp = run([&](auto cb) { controller.submit(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);
}

TEST_F(ContactConfiguredTest, MissingFieldsRejected) {
    json body = {{"name", "Ann"}};  // no email/message
    auto resp = run([&](auto cb) { controller.submit(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(ContactConfiguredTest, CrlfInSubjectRejected) {
    json body = {{"name", "Ann"},
                 {"email", "ann@example.com"},
                 {"subject", "Hi\r\nBcc: victim@example.com"},
                 {"message", "m"}};
    auto resp = run([&](auto cb) { controller.submit(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(ContactConfiguredTest, InvalidEmailRejected) {
    json body = {{"name", "Ann"}, {"email", "not-an-email"}, {"message", "m"}};
    auto resp = run([&](auto cb) { controller.submit(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// No recipient configured → endpoint reports it's disabled.
class ContactDisabledTest : public TestHelpers::CoreBackedTest {
protected:
    Api::ContactController controller;
    std::string config_file_name() const override { return "contact_disabled_test_config.json"; }
};

TEST_F(ContactDisabledTest, Returns503WhenUnconfigured) {
    json body = {{"name", "Ann"}, {"email", "ann@example.com"}, {"message", "m"}};
    auto resp = run([&](auto cb) { controller.submit(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k503ServiceUnavailable);
}

}  // namespace
