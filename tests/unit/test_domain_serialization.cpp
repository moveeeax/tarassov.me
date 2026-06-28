/**
 * @file test_domain_serialization.cpp
 * @brief Guards the domain DTO -> JSON contract (pure, no services).
 *
 * Each field is spelled out three times — struct, from_row, to_json — so the
 * one that matters for security (password_hash must NEVER be serialized) is
 * easy to drop by accident when adding a field. This test fails loudly if a
 * secret ever leaks into the public JSON.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "domain/Role.hpp"
#include "domain/User.hpp"

using json = nlohmann::json;

namespace {

Domain::User sample_user() {
    Domain::User u;
    u.id = "11111111-1111-1111-1111-111111111111";
    u.email = "alice@example.com";
    u.password_hash = "$argon2id$v=19$super-secret-hash";
    u.first_name = "Alice";
    u.last_name = "Smith";
    u.confirmed = true;
    u.role_id = 1;
    u.created_at = "2026-01-01 00:00:00+00";
    u.updated_at = "2026-01-01 00:00:00+00";
    return u;
}

TEST(DomainSerializationTest, UserJsonNeverLeaksPasswordHash) {
    json j = sample_user();
    const std::string dump = j.dump();
    EXPECT_FALSE(j.contains("password_hash")) << "password_hash must never be serialized";
    EXPECT_EQ(dump.find("password_hash"), std::string::npos);
    EXPECT_EQ(dump.find("super-secret-hash"), std::string::npos) << "secret value leaked into JSON";
}

TEST(DomainSerializationTest, UserJsonHasExpectedPublicFields) {
    json j = sample_user();
    EXPECT_EQ(j["email"], "alice@example.com");
    EXPECT_EQ(j["confirmed"], true);
    EXPECT_TRUE(j.contains("id"));
    EXPECT_TRUE(j.contains("created_at"));
}

TEST(DomainSerializationTest, RoleJsonRoundtripsBits) {
    Domain::Role r;
    r.id = 2;
    r.name = "Editor";
    r.permissions = 0x07;
    r.is_default = false;
    json j = r;
    EXPECT_EQ(j["name"], "Editor");
    EXPECT_EQ(j["permissions"].get<std::uint32_t>(), 0x07u);
}

}  // namespace
