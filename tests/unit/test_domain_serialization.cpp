/**
 * @file test_domain_serialization.cpp
 * @brief Guards the domain DTO -> JSON contract (pure, no services).
 *
 * Each field is spelled out three times — struct, from_row, to_json — so the
 * one that matters for security (password_hash must NEVER be serialized) is
 * easy to drop by accident when adding a field. This test fails loudly if a
 * secret ever leaks into the public JSON.
 */

#include <map>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "domain/AuditEntry.hpp"
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
    // ISO 8601, not libpqxx's "2026-01-01 00:00:00+00" text form: every
    // from_row() in src/domain normalizes through Utils::Time::pg_to_iso8601,
    // so this is the shape the field actually carries by the time to_json sees
    // it, and the shape docs/openapi.yaml declares (format: date-time).
    u.created_at = "2026-01-01T00:00:00Z";
    u.updated_at = "2026-01-01T00:00:00Z";
    return u;
}

// ---------------------------------------------------------------------------
// Minimal stand-in for a pqxx::row. from_row() only ever does
// row["col"].is_null() and row["col"].template as<T>(), so a map of the raw
// text values libpqxx would hand back reproduces the boundary faithfully
// enough to pin the serialization contract without a database — this stays in
// the unit bucket.
// ---------------------------------------------------------------------------

class FakeField {
public:
    explicit FakeField(const std::string* value) : value_(value) {}

    bool is_null() const { return value_ == nullptr; }

    template <typename T>
    T as() const {
        if constexpr (std::is_same_v<T, std::string>)
            return *value_;
        else
            return static_cast<T>(std::stoll(*value_));
    }

private:
    const std::string* value_;
};

class FakeRow {
public:
    explicit FakeRow(std::map<std::string, std::string> columns) : columns_(std::move(columns)) {}

    /// Absent key == SQL NULL, which is how the optional columns are spelled.
    FakeField operator[](const std::string& name) const {
        const auto it = columns_.find(name);
        return FakeField(it == columns_.end() ? nullptr : &it->second);
    }

private:
    std::map<std::string, std::string> columns_;
};

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

TEST(DomainSerializationTest, UserJsonTimestampsAreIso8601) {
    // docs/openapi.yaml declares created_at/updated_at as format: date-time.
    // libpqxx's "YYYY-MM-DD HH:MM:SS+00" is not that, so from_row normalizes —
    // this pins the shape the API is contractually allowed to emit.
    json j = sample_user();
    EXPECT_EQ(j["created_at"], "2026-01-01T00:00:00Z");
    EXPECT_EQ(j["updated_at"], "2026-01-01T00:00:00Z");
}

// ── AuditEntry::from_row ────────────────────────────────────────────────────

TEST(DomainSerializationTest, AuditEntryFromRowNormalizesTimestamp) {
    FakeRow row({{"id", "42"},
                 {"actor_id", "22222222-2222-2222-2222-222222222222"},
                 {"action", "user.create"},
                 {"target_type", "user"},
                 {"target_id", "33333333-3333-3333-3333-333333333333"},
                 {"details", R"({"email":"alice@example.com"})"},
                 // Exactly what libpqxx hands back for a timestamptz column.
                 {"created_at", "2026-07-26 05:34:32.876+00"}});

    auto a = Domain::AuditEntry::from_row(row);
    EXPECT_EQ(a.id, 42);
    EXPECT_EQ(a.action, "user.create");
    ASSERT_TRUE(a.actor_id.has_value());
    EXPECT_EQ(*a.actor_id, "22222222-2222-2222-2222-222222222222");
    EXPECT_EQ(a.created_at, "2026-07-26T05:34:32Z") << "raw Postgres timestamp leaked into the DTO";

    json j = a;
    EXPECT_EQ(j["created_at"], "2026-07-26T05:34:32Z");
    EXPECT_EQ(j["details"]["email"], "alice@example.com");
}

TEST(DomainSerializationTest, AuditEntryFromRowRendersNullsAsJsonNull) {
    // A system-generated entry has no actor and no target id.
    FakeRow row({{"id", "7"},
                 {"action", "system.startup"},
                 {"target_type", "system"},
                 {"created_at", "2026-07-26 05:34:32+00"}});

    auto a = Domain::AuditEntry::from_row(row);
    EXPECT_FALSE(a.actor_id.has_value());
    EXPECT_FALSE(a.target_id.has_value());
    EXPECT_TRUE(a.details.is_object());
    EXPECT_TRUE(a.details.empty());

    json j = a;
    EXPECT_TRUE(j["actor_id"].is_null());
    EXPECT_TRUE(j["target_id"].is_null());
    EXPECT_EQ(j["created_at"], "2026-07-26T05:34:32Z");
}

TEST(DomainSerializationTest, AuditEntryFromRowSurvivesUnparsableDetails) {
    // details is a text column; a hand-edited or truncated row must degrade to
    // an empty object rather than throw out of the admin audit listing.
    FakeRow row({{"id", "8"},
                 {"action", "user.update"},
                 {"target_type", "user"},
                 {"details", "{not json"},
                 {"created_at", "2026-07-26 05:34:32+00"}});

    auto a = Domain::AuditEntry::from_row(row);
    EXPECT_TRUE(a.details.is_object());
    EXPECT_TRUE(a.details.empty());
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
