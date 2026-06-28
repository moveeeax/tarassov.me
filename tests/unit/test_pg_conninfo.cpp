/**
 * @file test_pg_conninfo.cpp
 * @brief Unit tests for Utils::Pg::make_conninfo — assemble a libpq key=value
 *        DSN from parts so the password stays out of a URL env var.
 */

#include <gtest/gtest.h>

#include "utils/Pg.hpp"

TEST(PgConninfoTest, AssemblesAllParts) {
    EXPECT_EQ(Utils::Pg::make_conninfo("db.internal", 5432, "app", "appdb", "s3cret"),
              "host='db.internal' port=5432 user='app' dbname='appdb' password='s3cret'");
}

TEST(PgConninfoTest, OmitsEmptyPassword) {
    // Peer / certificate auth: no password component at all.
    EXPECT_EQ(Utils::Pg::make_conninfo("/var/run/postgresql", 5432, "app", "appdb", ""),
              "host='/var/run/postgresql' port=5432 user='app' dbname='appdb'");
}

TEST(PgConninfoTest, EscapesQuotesAndBackslashesInPassword) {
    // A password with a single quote and a backslash must be escaped, not break
    // out of the value.
    EXPECT_EQ(Utils::Pg::make_conninfo("h", 5432, "u", "d", "a'b\\c"),
              "host='h' port=5432 user='u' dbname='d' password='a\\'b\\\\c'");
}
