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

// ── require_string ──────────────────────────────────────────────────────────
// require() only proves presence, so a wrong-typed field used to sail through
// and blow up in the handler's body[f].get<std::string>() as nlohmann's
// type_error.302 — a bare 500 (on unauthenticated paths too) instead of the
// 400 envelope. These pin the type check itself.

TEST(ValidationTest, RequireStringAcceptsString) {
    V::Errors errs;
    json body = {{"password", "hunter2hunter2"}};
    EXPECT_TRUE(V::require_string(errs, body, "password"));
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, RequireStringRejectsNonString) {
    V::Errors errs;
    json body = {{"password", 123}};
    EXPECT_FALSE(V::require_string(errs, body, "password"));
    ASSERT_EQ(errs.items().size(), 1u);
    EXPECT_EQ(errs.items()[0].field, "password");
    EXPECT_EQ(errs.items()[0].code, "not_string");
}

TEST(ValidationTest, RequireStringRejectsStructuredValues) {
    // An object or array is the other shape a hostile client sends to reach
    // get<std::string>() — both must land as not_string, not as a 500.
    for (const json& value : {json::object(), json::array({1, 2}), json(true)}) {
        V::Errors errs;
        json body = {{"email", value}};
        EXPECT_FALSE(V::require_string(errs, body, "email")) << value.dump();
        ASSERT_EQ(errs.items().size(), 1u) << value.dump();
        EXPECT_EQ(errs.items()[0].code, "not_string") << value.dump();
    }
}

TEST(ValidationTest, RequireStringReportsMissingOnceNotTwice) {
    // Absent / null short-circuits on require(): one "missing", never a
    // "missing" + "not_string" pair for the same field.
    for (const json& body : {json::object(), json{{"password", nullptr}}}) {
        V::Errors errs;
        EXPECT_FALSE(V::require_string(errs, body, "password")) << body.dump();
        ASSERT_EQ(errs.items().size(), 1u) << body.dump();
        EXPECT_EQ(errs.items()[0].code, "missing") << body.dump();
    }
}

TEST(ValidationTest, RequireStringAcceptsEmptyString) {
    // Presence and type only — length is a separate validator's job.
    V::Errors errs;
    json body = {{"name", ""}};
    EXPECT_TRUE(V::require_string(errs, body, "name"));
    EXPECT_FALSE(errs.any());
}

// ── boolean ─────────────────────────────────────────────────────────────────
// body.value(f, false) throws type_error.302 on a non-boolean, so every
// optional flag needs this gate before it is read.

TEST(ValidationTest, BooleanAcceptsBothLiterals) {
    for (bool v : {true, false}) {
        V::Errors errs;
        json body = {{"is_default", v}};
        V::boolean(errs, body, "is_default");
        EXPECT_FALSE(errs.any()) << v;
    }
}

TEST(ValidationTest, BooleanRejectsNonBoolean) {
    // The string "true" and the number 1 are the two values clients actually
    // send instead of a JSON boolean.
    for (const json& value : {json("true"), json(1), json::array(), json::object()}) {
        V::Errors errs;
        json body = {{"is_default", value}};
        V::boolean(errs, body, "is_default");
        ASSERT_EQ(errs.items().size(), 1u) << value.dump();
        EXPECT_EQ(errs.items()[0].field, "is_default") << value.dump();
        EXPECT_EQ(errs.items()[0].code, "not_boolean") << value.dump();
    }
}

TEST(ValidationTest, BooleanIsNoOpForMissingAndNull) {
    // Absent == explicit null == "leave it at the default" for the callers
    // (AdminController's is_default), so neither may raise.
    for (const json& body : {json::object(), json{{"is_default", nullptr}}}) {
        V::Errors errs;
        V::boolean(errs, body, "is_default");
        EXPECT_FALSE(errs.any()) << body.dump();
    }
}

// ── person_name ─────────────────────────────────────────────────────────────
// Composite: length cap (users.first_name / last_name are VARCHAR(64), and an
// over-long value used to come back as SQLSTATE 22001 → 500), no CRLF (mail
// header injection) and no HTML-significant characters (the transactional
// templates render names through inja).

TEST(ValidationTest, PersonNameAcceptsOrdinaryNames) {
    for (const char* name : {"Alice", "O'Brien", "Anne-Marie", "van der Berg", "Ada Lovelace", "Ångström"}) {
        V::Errors errs;
        json body = {{"last_name", name}};
        V::person_name(errs, body, "last_name");
        EXPECT_FALSE(errs.any()) << name << " must be a legal name";
    }
}

TEST(ValidationTest, PersonNameIsOptional) {
    for (const json& body : {json::object(), json{{"first_name", nullptr}}}) {
        V::Errors errs;
        V::person_name(errs, body, "first_name");
        EXPECT_FALSE(errs.any()) << body.dump();
    }
}

TEST(ValidationTest, PersonNameAcceptsExactlyTheColumnWidth) {
    V::Errors errs;
    json body = {{"first_name", std::string(64, 'x')}};  // VARCHAR(64) — the boundary
    V::person_name(errs, body, "first_name");
    EXPECT_FALSE(errs.any());
}

TEST(ValidationTest, PersonNameRejectsOverLength) {
    V::Errors errs;
    json body = {{"first_name", std::string(65, 'x')}};
    V::person_name(errs, body, "first_name");
    ASSERT_EQ(errs.items().size(), 1u);
    EXPECT_EQ(errs.items()[0].field, "first_name");
    EXPECT_EQ(errs.items()[0].code, "too_long");
}

TEST(ValidationTest, PersonNameHonoursACustomCap) {
    V::Errors errs;
    json body = {{"first_name", "abcdefghij"}};
    V::person_name(errs, body, "first_name", /*max_len=*/5);
    ASSERT_EQ(errs.items().size(), 1u);
    EXPECT_EQ(errs.items()[0].code, "too_long");
}

TEST(ValidationTest, PersonNameRejectsHtmlSignificantCharacters) {
    for (const char* payload : {"<script>alert(1)</script>", "Tom & Jerry", "a > b"}) {
        V::Errors errs;
        json body = {{"last_name", payload}};
        V::person_name(errs, body, "last_name");
        ASSERT_EQ(errs.items().size(), 1u) << payload;
        EXPECT_EQ(errs.items()[0].field, "last_name") << payload;
        EXPECT_EQ(errs.items()[0].code, "invalid") << payload;
    }
}

TEST(ValidationTest, PersonNameRejectsCrlf) {
    // Header-injection shape: the name lands in a mail To:/Subject: line.
    for (const char* payload : {"Alice\r\nBcc: evil@example.com", "Alice\nX-Evil: 1", "Alice\rBob"}) {
        V::Errors errs;
        json body = {{"first_name", payload}};
        V::person_name(errs, body, "first_name");
        ASSERT_EQ(errs.items().size(), 1u) << payload;
        EXPECT_EQ(errs.items()[0].code, "invalid") << payload;
    }
}

TEST(ValidationTest, PersonNameRejectsNonString) {
    V::Errors errs;
    json body = {{"first_name", 42}};
    V::person_name(errs, body, "first_name");
    // string_length reports the type; no_crlf / no_html are no-ops on a
    // non-string, so exactly one error — not three.
    ASSERT_EQ(errs.items().size(), 1u);
    EXPECT_EQ(errs.items()[0].code, "not_string");
}

TEST(ValidationTest, PersonNameAccumulatesEveryViolation) {
    // Long AND markup-bearing: both sub-validators must report, so the client
    // gets one round-trip instead of a fix-one-find-another loop.
    V::Errors errs;
    json body = {{"last_name", std::string(70, 'x') + "<b>"}};
    V::person_name(errs, body, "last_name");
    ASSERT_EQ(errs.items().size(), 2u);
    EXPECT_EQ(errs.items()[0].code, "too_long");
    EXPECT_EQ(errs.items()[1].code, "invalid");
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
