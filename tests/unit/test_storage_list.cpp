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

// ── S3 ListObjectsV2 parsing (pure, canned responses — no network) ──────────

TEST(StorageList, S3ParsesListObjectsV2) {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>bkt</Name><Prefix>posts/</Prefix><KeyCount>2</KeyCount>
  <IsTruncated>false</IsTruncated>
  <Contents>
    <Key>posts/old.png</Key>
    <LastModified>2026-07-01T10:00:00.000Z</LastModified>
    <ETag>&quot;abc&quot;</ETag>
    <Size>1234</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <Contents>
    <Key>posts/new.jpg</Key>
    <LastModified>2026-07-25T05:34:32.000Z</LastModified>
    <Size>99</Size>
  </Contents>
</ListBucketResult>)";
    bool truncated = true;
    auto items = Storage::S3Storage::parse_list_objects_xml(xml, &truncated);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_FALSE(truncated);
    // Newest first; millisecond suffix normalized to ISO seconds + Z.
    EXPECT_EQ(items[0].key, "posts/new.jpg");
    EXPECT_EQ(items[0].last_modified, "2026-07-25T05:34:32Z");
    EXPECT_EQ(items[0].size_bytes, 99u);
    EXPECT_EQ(items[1].key, "posts/old.png");
    EXPECT_EQ(items[1].size_bytes, 1234u);
}

TEST(StorageList, S3ReportsTruncationAndSurvivesMalformedEntries) {
    const std::string xml =
        "<ListBucketResult><IsTruncated>true</IsTruncated>"
        "<Contents><Key>posts/ok.png</Key><Size>5</Size>"
        "<LastModified>2026-07-25T00:00:00.000Z</LastModified></Contents>"
        "<Contents><Size>7</Size></Contents>"  // no <Key> → dropped, not crashed
        "<Contents><Key>posts/nosize.png</Key></Contents>"
        "</ListBucketResult>";
    bool truncated = false;
    auto items = Storage::S3Storage::parse_list_objects_xml(xml, &truncated);
    EXPECT_TRUE(truncated);
    ASSERT_EQ(items.size(), 2u);  // keyless entry dropped
    // Missing Size/LastModified degrade to 0/empty rather than throwing.
    bool found_nosize = false;
    for (const auto& o : items)
        if (o.key == "posts/nosize.png") {
            found_nosize = true;
            EXPECT_EQ(o.size_bytes, 0u);
            EXPECT_TRUE(o.last_modified.empty());
        }
    EXPECT_TRUE(found_nosize);
}

TEST(StorageList, S3EmptyBucketParsesToEmpty) {
    bool truncated = true;
    EXPECT_TRUE(
        Storage::S3Storage::parse_list_objects_xml(
            "<ListBucketResult><KeyCount>0</KeyCount><IsTruncated>false</IsTruncated></ListBucketResult>", &truncated)
            .empty());
    EXPECT_FALSE(truncated);
}
