/**
 * @file test_pg_conninfo.cpp
 * @brief Unit tests for Utils::Pg::make_conninfo — assemble a libpq key=value
 *        DSN from parts so the password stays out of a URL env var.
 */

#include <gtest/gtest.h>

#include "utils/Pg.hpp"

TEST(PgConninfoTest, AssemblesAllParts) {
    // connect_timeout=5 is appended by default: an unbounded libpq connect
    // against a blackholed endpoint would park a thread for ~2 minutes.
    EXPECT_EQ(Utils::Pg::make_conninfo("db.internal", 5432, "app", "appdb", "s3cret"),
              "host='db.internal' port=5432 user='app' dbname='appdb' password='s3cret' connect_timeout=5");
}

TEST(PgConninfoTest, OmitsEmptyPassword) {
    // Peer / certificate auth: no password component at all.
    EXPECT_EQ(Utils::Pg::make_conninfo("/var/run/postgresql", 5432, "app", "appdb", ""),
              "host='/var/run/postgresql' port=5432 user='app' dbname='appdb' connect_timeout=5");
}

TEST(PgConninfoTest, EscapesQuotesAndBackslashesInPassword) {
    // A password with a single quote and a backslash must be escaped, not break
    // out of the value.
    EXPECT_EQ(Utils::Pg::make_conninfo("h", 5432, "u", "d", "a'b\\c"),
              "host='h' port=5432 user='u' dbname='d' password='a\\'b\\\\c' connect_timeout=5");
}

TEST(PgConninfoTest, ConnectTimeoutZeroDisables) {
    EXPECT_EQ(Utils::Pg::make_conninfo("h", 5432, "u", "d", "", 0), "host='h' port=5432 user='u' dbname='d'");
}

TEST(PgConninfoTest, HasConnectTimeoutDetectsBothForms) {
    EXPECT_TRUE(Utils::Pg::has_connect_timeout("host='h' connect_timeout=3"));
    EXPECT_TRUE(Utils::Pg::has_connect_timeout("postgresql://u@h/db?connect_timeout=3"));
    EXPECT_FALSE(Utils::Pg::has_connect_timeout("postgresql://u@h/db"));
}

TEST(PgConninfoTest, WithConnectTimeoutAppendsPerForm) {
    // key=value form → space kv.
    EXPECT_EQ(Utils::Pg::with_connect_timeout("host='h' dbname='d'"), "host='h' dbname='d' connect_timeout=5");
    // URL without query → ?connect_timeout.
    EXPECT_EQ(Utils::Pg::with_connect_timeout("postgresql://u@h/db"), "postgresql://u@h/db?connect_timeout=5");
    // URL with existing query → &connect_timeout.
    EXPECT_EQ(Utils::Pg::with_connect_timeout("postgres://u@h/db?sslmode=require"),
              "postgres://u@h/db?sslmode=require&connect_timeout=5");
    // Already pinned → untouched (both forms).
    EXPECT_EQ(Utils::Pg::with_connect_timeout("postgresql://u@h/db?connect_timeout=9"),
              "postgresql://u@h/db?connect_timeout=9");
    EXPECT_EQ(Utils::Pg::with_connect_timeout("host='h' connect_timeout=9"), "host='h' connect_timeout=9");
    // Disabled.
    EXPECT_EQ(Utils::Pg::with_connect_timeout("host='h'", 0), "host='h'");
}
