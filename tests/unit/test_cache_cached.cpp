/**
 * @file test_cache_cached.cpp
 * @brief Cache::cached read-through helper — no infra (InMemoryCache fake).
 */

#include <memory>

#include <gtest/gtest.h>

#include "InMemoryCache.hpp"
#include "cache/Cache.hpp"

namespace {

struct CacheCachedTest : ::testing::Test {
    void SetUp() override { Cache::install_for_testing(std::make_unique<TestSupport::InMemoryCache>()); }
    void TearDown() override { Cache::reset_for_testing(); }
};

TEST_F(CacheCachedTest, LoaderRunsOnceThenServesFromCache) {
    int calls = 0;
    auto load = [&] {
        ++calls;
        return 42;
    };
    EXPECT_EQ(Cache::cached<int>("k", 60, load), 42);
    EXPECT_EQ(Cache::cached<int>("k", 60, load), 42);  // served from cache
    EXPECT_EQ(calls, 1);
}

TEST_F(CacheCachedTest, DistinctKeysLoadIndependently) {
    int calls = 0;
    EXPECT_EQ(Cache::cached<int>("a",
                                 60,
                                 [&] {
                                     ++calls;
                                     return 1;
                                 }),
              1);
    EXPECT_EQ(Cache::cached<int>("b",
                                 60,
                                 [&] {
                                     ++calls;
                                     return 2;
                                 }),
              2);
    EXPECT_EQ(calls, 2);
}

TEST_F(CacheCachedTest, RoundTripsAComplexType) {
    auto load = [] { return std::vector<std::string>{"x", "y", "z"}; };
    EXPECT_EQ(Cache::cached<std::vector<std::string>>("v", 60, load), (std::vector<std::string>{"x", "y", "z"}));
    int calls = 0;
    auto reload = [&] {
        ++calls;
        return std::vector<std::string>{};
    };
    // Second call hits the cache and ignores the (different) reloader.
    EXPECT_EQ(Cache::cached<std::vector<std::string>>("v", 60, reload), (std::vector<std::string>{"x", "y", "z"}));
    EXPECT_EQ(calls, 0);
}

// Fail-open: with no cache installed, the helper still returns the loaded value
// (correctness must never depend on Redis being up) — it just can't memoize.
TEST(CacheCachedFailOpen, FallsThroughWhenCacheUninitialized) {
    Cache::reset_for_testing();
    int calls = 0;
    auto load = [&] {
        ++calls;
        return 7;
    };
    EXPECT_EQ(Cache::cached<int>("k", 60, load), 7);
    EXPECT_EQ(Cache::cached<int>("k", 60, load), 7);
    EXPECT_EQ(calls, 2);
}

}  // namespace
