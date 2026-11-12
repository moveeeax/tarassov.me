/**
 * @file test_atom_feed.cpp
 * @brief Unit tests for the Atom feed renderer and its cache-generation
 *        helpers (src/pages/AtomFeed.hpp). No infra: the cache side runs
 *        against the InMemoryCache fake.
 */

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "InMemoryCache.hpp"
#include "cache/Cache.hpp"
#include "pages/AtomFeed.hpp"

namespace {

Pages::AtomFeedInfo info() {
    Pages::AtomFeedInfo i;
    i.base = "https://example.test";
    i.id = "https://example.test/feed.xml";
    i.self_url = i.id;
    i.html_url = "https://example.test/blog.html";
    i.title = "Notes";
    i.subtitle = "Sub";
    i.author = "Michael Tarassov";
    i.empty_updated = "2026-01-01T00:00:00Z";
    return i;
}

Domain::PostFeedItem item(const std::string& slug, const std::string& updated) {
    Domain::PostFeedItem f;
    f.id = "11111111-1111-1111-1111-111111111111";
    f.slug = slug;
    f.title = "Title " + slug;
    f.summary = "Summary " + slug;
    f.published_at = "2026-02-01T00:00:00Z";
    f.updated_at = updated;
    return f;
}

std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (auto p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

TEST(AtomFeed, RendersRequiredFeedAndEntryElements) {
    auto e = item("hello", "2026-03-01T10:00:00Z");
    e.topic = "Kubernetes";
    e.tags = {"k8s", "talos"};
    const auto xml = Pages::render_atom(info(), {e});

    EXPECT_EQ(xml.rfind("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n", 0), 0u);
    EXPECT_NE(xml.find("<feed xmlns=\"http://www.w3.org/2005/Atom\">"), std::string::npos);
    EXPECT_NE(xml.find("<id>https://example.test/feed.xml</id>"), std::string::npos);
    EXPECT_NE(xml.find("<link rel=\"self\" type=\"application/atom+xml\" href=\"https://example.test/feed.xml\"/>"),
              std::string::npos);
    EXPECT_NE(xml.find("<link rel=\"alternate\" type=\"text/html\" href=\"https://example.test/blog.html\"/>"),
              std::string::npos);
    EXPECT_NE(xml.find("<author><name>Michael Tarassov</name>"), std::string::npos);

    EXPECT_NE(xml.find("<id>urn:uuid:" + e.id + "</id>"), std::string::npos);
    EXPECT_NE(xml.find("href=\"https://example.test/blog/hello\""), std::string::npos);
    EXPECT_NE(xml.find("<published>2026-02-01T00:00:00Z</published>"), std::string::npos);
    EXPECT_NE(xml.find("<summary type=\"text\">Summary hello</summary>"), std::string::npos);
    EXPECT_NE(xml.find("<category term=\"Kubernetes\"/>"), std::string::npos);
    EXPECT_NE(xml.find("<category term=\"k8s\"/>"), std::string::npos);
    EXPECT_NE(xml.find("<category term=\"talos\"/>"), std::string::npos);
    EXPECT_EQ(xml.substr(xml.size() - 8), "</feed>\n");
}

TEST(AtomFeed, FeedUpdatedIsNewestEntryUpdate) {
    // Entries arrive in publish order; an edit to an OLDER post is the newest change.
    const auto xml =
        Pages::render_atom(info(), {item("new", "2026-03-01T00:00:00Z"), item("old", "2026-04-05T12:00:00Z")});
    const auto feed_updated = xml.find("<updated>");
    const auto first_entry = xml.find("<entry>");
    ASSERT_LT(feed_updated, first_entry);
    EXPECT_EQ(xml.substr(feed_updated, 40), "<updated>2026-04-05T12:00:00Z</updated>\n");
}

TEST(AtomFeed, EmptyFeedUsesFallbackUpdatedAndHasNoEntries) {
    const auto xml = Pages::render_atom(info(), {});
    EXPECT_NE(xml.find("<updated>2026-01-01T00:00:00Z</updated>"), std::string::npos);
    EXPECT_EQ(xml.find("<entry>"), std::string::npos);
}

TEST(AtomFeed, EscapesMarkupAndDropsForbiddenControlChars) {
    auto e = item("esc", "2026-03-01T00:00:00Z");
    e.title = "<script>&\"'";
    e.summary = std::string("tab\there\x0b vt\x01 nul-free");
    e.tags = {"a\"b"};
    const auto xml = Pages::render_atom(info(), {e});
    EXPECT_NE(xml.find("<title type=\"text\">&lt;script&gt;&amp;&quot;&#39;</title>"), std::string::npos);
    EXPECT_NE(xml.find("<summary type=\"text\">tab\there vt nul-free</summary>"), std::string::npos);
    EXPECT_NE(xml.find("<category term=\"a&quot;b\"/>"), std::string::npos);
    EXPECT_EQ(xml.find("<script>"), std::string::npos);
}

TEST(AtomFeed, OmitsEmptySummaryAndDedupesTopicTag) {
    auto e = item("bare", "2026-03-01T00:00:00Z");
    e.summary.clear();
    e.published_at.reset();
    e.topic = "SRE";
    e.tags = {"SRE", "oncall"};
    const auto xml = Pages::render_atom(info(), {e});
    EXPECT_EQ(xml.find("<summary"), std::string::npos);
    EXPECT_EQ(xml.find("<published>"), std::string::npos);
    EXPECT_EQ(count(xml, "<category term=\"SRE\"/>"), 1u);
    EXPECT_EQ(count(xml, "<category term=\"oncall\"/>"), 1u);
}

TEST(AtomFeed, UrlEncodeComponentKeepsUnreservedOnly) {
    EXPECT_EQ(Pages::url_encode_component("Kubernetes"), "Kubernetes");
    EXPECT_EQ(Pages::url_encode_component("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
    EXPECT_EQ(Pages::url_encode_component("-._~"), "-._~");
}

struct AtomFeedCacheTest : ::testing::Test {
    void SetUp() override { Cache::install_for_testing(std::make_unique<TestSupport::InMemoryCache>()); }
    void TearDown() override { Cache::reset_for_testing(); }
};

TEST_F(AtomFeedCacheTest, InvalidationMovesEveryTopicToANewKey) {
    const auto main_before = Pages::feed_cache_key("");
    const auto topic_before = Pages::feed_cache_key("Kubernetes");
    EXPECT_NE(main_before, topic_before);
    EXPECT_EQ(Pages::feed_cache_key(""), main_before);  // stable until a write

    Pages::invalidate_feed_cache();
    EXPECT_NE(Pages::feed_cache_key(""), main_before);
    EXPECT_NE(Pages::feed_cache_key("Kubernetes"), topic_before);
}

TEST(AtomFeedCacheFailOpen, WorksWithoutCache) {
    Cache::reset_for_testing();
    EXPECT_EQ(Pages::feed_cache_key("x"), "feed:atom:0:x");
    EXPECT_NO_THROW(Pages::invalidate_feed_cache());
}

}  // namespace
