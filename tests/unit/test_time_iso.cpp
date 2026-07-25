/**
 * @file test_time_iso.cpp
 * @brief Unit tests for Utils::Time ISO 8601 helpers — the serialization
 *        boundary converts libpqxx's "YYYY-MM-DD HH:MM:SS.ffffff+00" text
 *        timestamps into API-facing ISO 8601 UTC.
 */

#include <gtest/gtest.h>

#include "utils/Time.hpp"

TEST(TimeIso, PgTimestampToIso) {
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32.87643+00"), "2026-07-25T05:34:32Z");
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32+00"), "2026-07-25T05:34:32Z");
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32+00:00"), "2026-07-25T05:34:32Z");
    // Non-UTC offset: keep the offset (still valid ISO 8601), normalize the shape.
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25 05:34:32+03"), "2026-07-25T05:34:32+03");
    // Already ISO or unexpected shape: returned unchanged.
    EXPECT_EQ(Utils::Time::pg_to_iso8601("2026-07-25T05:34:32Z"), "2026-07-25T05:34:32Z");
    EXPECT_EQ(Utils::Time::pg_to_iso8601(""), "");
}

TEST(TimeIso, EpochToIso) {
    EXPECT_EQ(Utils::Time::epoch_to_iso8601(0), "1970-01-01T00:00:00Z");
    EXPECT_EQ(Utils::Time::epoch_to_iso8601(1774417272), "2026-03-25T05:41:12Z");
}
