#include <gtest/gtest.h>

#include "api/Validation.hpp"

namespace V = Api::Validation;
using json = nlohmann::json;

TEST(ValidationTest, RequireMissing) {
    V::Errors errs;
    json body = {{"other", 1}};
    EXPECT_FALSE(V::require(errs, body, "needed"));
    ASSERT_EQ(errs.items().size(), 1u);
    EXPECT_EQ(errs.items()[0].field, "needed");
    EXPECT_EQ(errs.items()[0].code, "missing");
}

TEST(ValidationTest, RequireNullIsMissing) {
    V::Errors errs;
    json body = {{"x", nullptr}};
    EXPECT_FALSE(V::require(errs, body, "x"));
    EXPECT_EQ(errs.items()[0].code, "missing");
}

TEST(ValidationTest, StringLengthInRange) {
    V::Errors errs;
    json body = {{"name", "abc"}};
    V::string_length(errs, body, "name", 1, 10);
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, StringTooShort) {
    V::Errors errs;
    json body = {{"name", ""}};
    V::string_length(errs, body, "name", 1, 10);
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "too_short");
}

TEST(ValidationTest, StringTooLong) {
    V::Errors errs;
    json body = {{"name", std::string(11, 'x')}};
    V::string_length(errs, body, "name", 1, 10);
    EXPECT_EQ(errs.items()[0].code, "too_long");
}

TEST(ValidationTest, IntRangeOk) {
    V::Errors errs;
    json body = {{"age", 25}};
    V::int_range(errs, body, "age", 0, 150);
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, IntRangeBelow) {
    V::Errors errs;
    json body = {{"age", -1}};
    V::int_range(errs, body, "age", 0, 150);
    EXPECT_EQ(errs.items()[0].code, "below_min");
}

TEST(ValidationTest, OneOfOk) {
    V::Errors errs;
    json body = {{"status", "draft"}};
    V::one_of(errs, body, "status", {"draft", "published", "archived"});
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, OneOfNotAllowed) {
    V::Errors errs;
    json body = {{"status", "whatever"}};
    V::one_of(errs, body, "status", {"draft", "published"});
    EXPECT_EQ(errs.items()[0].code, "not_allowed");
}

TEST(ValidationTest, EmailGood) {
    V::Errors errs;
    json body = {{"email", "foo@bar.com"}};
    V::email(errs, body, "email");
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, EmailBad) {
    V::Errors errs;
    json body = {{"email", "not-an-email"}};
    V::email(errs, body, "email");
    EXPECT_EQ(errs.items()[0].code, "bad_format");
}

TEST(ValidationTest, UuidGood) {
    V::Errors errs;
    json body = {{"id", "550e8400-e29b-41d4-a716-446655440000"}};
    V::uuid(errs, body, "id");
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, UuidBad) {
    V::Errors errs;
    json body = {{"id", "not-a-uuid"}};
    V::uuid(errs, body, "id");
    ASSERT_TRUE(errs.any());
    EXPECT_EQ(errs.items()[0].code, "bad_format");
}

TEST(ValidationTest, AccumulatesMultiple) {
    V::Errors errs;
    json body = {{"name", ""}, {"age", 200}};
    V::string_length(errs, body, "name", 1, 10);
    V::int_range(errs, body, "age", 0, 150);
    V::require(errs, body, "missing_field");
    EXPECT_EQ(errs.items().size(), 3u);
    auto arr = errs.errors_json();
    EXPECT_TRUE(arr.is_array());
    EXPECT_EQ(arr.size(), 3u);

    // The full response body goes through the shared ErrorResponse helper.
    auto resp = V::response_400(errs);
    EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
    auto resp_body = nlohmann::json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["error"], "validation_failed");
    EXPECT_EQ(resp_body["status"], 400);
    EXPECT_EQ(resp_body["errors"].size(), 3u);
}
