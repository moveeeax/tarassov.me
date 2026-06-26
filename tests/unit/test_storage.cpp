/**
 * @file test_storage.cpp
 * @brief LocalStorage backend — put/get/remove round-trip + path-traversal
 *        defense. No infra (a temp directory).
 */

#include <filesystem>
#include <memory>
#include <system_error>

#include <gtest/gtest.h>

#include "storage/Storage.hpp"

namespace {

namespace fs = std::filesystem;

struct LocalStorageTest : ::testing::Test {
    fs::path root;
    std::unique_ptr<Storage::LocalStorage> store;

    void SetUp() override {
        std::error_code ec;
        root = fs::temp_directory_path() / "cpp_storage_test";
        fs::remove_all(root, ec);
        store = std::make_unique<Storage::LocalStorage>(root, "https://cdn.example.com/files");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

TEST_F(LocalStorageTest, PutGetRemoveRoundTrip) {
    EXPECT_FALSE(store->exists("a/b.txt"));
    store->put("a/b.txt", "hello world", "text/plain");
    EXPECT_TRUE(store->exists("a/b.txt"));

    auto got = store->get("a/b.txt");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "hello world");

    EXPECT_TRUE(store->remove("a/b.txt"));
    EXPECT_FALSE(store->exists("a/b.txt"));
    EXPECT_FALSE(store->get("a/b.txt").has_value());
}

TEST_F(LocalStorageTest, UrlJoinsPublicBase) {
    EXPECT_EQ(store->url("x/y.png"), "https://cdn.example.com/files/x/y.png");
}

TEST_F(LocalStorageTest, RefusesPathTraversalAndUnsafeKeys) {
    EXPECT_THROW(store->put("../escape", "x", "text/plain"), std::runtime_error);
    EXPECT_THROW(store->get("/etc/passwd"), std::runtime_error);
    EXPECT_THROW(store->url("a/../../b"), std::runtime_error);

    EXPECT_FALSE(Storage::key_is_safe(""));
    EXPECT_FALSE(Storage::key_is_safe("/abs"));
    EXPECT_FALSE(Storage::key_is_safe("a/../b"));
    EXPECT_TRUE(Storage::key_is_safe("uuid-123/file.png"));
}

}  // namespace
