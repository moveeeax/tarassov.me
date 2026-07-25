/**
 * @file test_storage_list.cpp
 * @brief Unit tests for StorageBackend::list() on the local backend — the
 *        media-library surface: prefix-scoped, newest-first, sizes populated.
 */

#include <filesystem>

#include <gtest/gtest.h>

#include "storage/Storage.hpp"

TEST(StorageList, LocalListsPrefixOnly) {
    const auto root = std::filesystem::temp_directory_path() / "storage-list-test";
    std::filesystem::remove_all(root);
    Storage::LocalStorage s(root, "http://cdn.test");
    s.put("posts/aaa.png", "x", "image/png");
    s.put("posts/bbb.jpg", "yy", "image/jpeg");
    s.put("other/ccc.png", "z", "image/png");

    auto items = s.list("posts/");
    ASSERT_EQ(items.size(), 2u);
    for (const auto& o : items) {
        EXPECT_EQ(o.key.rfind("posts/", 0), 0u);
        EXPECT_GT(o.size_bytes, 0u);
        EXPECT_NE(o.last_modified.find('T'), std::string::npos);  // ISO 8601
    }

    // Unknown prefix → empty, not an error.
    EXPECT_TRUE(s.list("nope/").empty());

    std::filesystem::remove_all(root);
}
