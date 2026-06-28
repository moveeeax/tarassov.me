/**
 * @file test_di_fake.cpp
 * @brief Demonstrates the DI/fake seam (#39): the cache singleton can be swapped
 *        for an in-memory fake, so cache-aside / lock / fail-open logic is
 *        unit-testable WITHOUT a live Redis. Pure unit — no Postgres/Redis.
 */

#include <chrono>
#include <memory>

#include <gtest/gtest.h>

#include "InMemoryCache.hpp"
#include "cache/Cache.hpp"

class DiFakeTest : public ::testing::Test {
protected:
    void SetUp() override { Cache::install_for_testing(std::make_unique<TestSupport::InMemoryCache>()); }
    void TearDown() override { Cache::reset_for_testing(); }
};

TEST_F(DiFakeTest, FakeCacheUsableWithoutRedis) {
    EXPECT_TRUE(Cache::is_initialized());
    auto& c = Cache::get();
    EXPECT_FALSE(c.get("k").has_value());
    EXPECT_TRUE(c.set("k", "v"));
    EXPECT_EQ(c.get("k").value_or(""), "v");
    EXPECT_TRUE(c.exists("k"));
    EXPECT_EQ(c.del("k"), 1);
    EXPECT_FALSE(c.exists("k"));
}

TEST_F(DiFakeTest, SetNxIsOneShotLock) {
    auto& c = Cache::get();
    EXPECT_TRUE(c.set_nx("lock", "1", std::chrono::seconds(60)));   // first caller wins
    EXPECT_FALSE(c.set_nx("lock", "2", std::chrono::seconds(60)));  // replay loses
}

TEST_F(DiFakeTest, IncrCounts) {
    auto& c = Cache::get();
    EXPECT_EQ(c.incr("n"), 1);
    EXPECT_EQ(c.incr("n", 5), 6);
    EXPECT_EQ(c.decr("n", 2), 4);
}
