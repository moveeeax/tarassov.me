/**
 * @file test_post.cpp
 * @brief Integration tests for PostsController — admin CRUD + the public
 *        read endpoints (published-only). Needs the posts migration (006).
 *
 * Lives in tests/integration/, globbed by CMake CONFIGURE_DEPENDS. With
 * AUTH_MODE=none the admin guard is a no-op, so the admin handlers are reachable.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/PostsController.hpp"
#include "api/PublicPagesController.hpp"
#include "security/Auth.hpp"
#include "security/Tokens.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

class PostsFlowTest : public TestHelpers::CoreBackedTest {
protected:
    Api::PostsController controller;
    std::string config_file_name() const override { return "post_flow_test_config.json"; }
    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        // Preview tokens are HMAC-signed with the auth secret; AUTH_MODE stays
        // none (admin guard no-op) but the signer still needs a key.
        cfg["auth"]["jwt"]["secret"] = "test-preview-secret-0123456789abcdef";
    }

    // Helper: call a (req, cb) handler and return the response.
    template <typename Fn>
    HttpResponsePtr call(Fn&& fn) {
        HttpResponsePtr resp;
        fn([&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }

    // Seed helper: create a published post via the admin handler.
    void seed(const char* slug, const char* title, const char* topic, json tags) {
        json body = {{"slug", slug},
                     {"title", title},
                     {"summary", std::string("about ") + title},
                     {"status", "published"},
                     {"topic", topic},
                     {"tags", std::move(tags)}};
        auto r = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
        ASSERT_EQ(r->statusCode(), k201Created);
    }
};

TEST_F(PostsFlowTest, ListReturnsEnvelope) {
    auto resp = call([&](auto cb) { controller.listPosts(TestHelpers::make_request(Get), std::move(cb)); });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_TRUE(body.contains("data"));
    EXPECT_TRUE(body.contains("total"));
}

TEST_F(PostsFlowTest, CreatePublishAndPublicRead) {
    json body = {{"slug", "hello-test"},
                 {"title", "Hello"},
                 {"summary", "s"},
                 {"body", "# h"},
                 {"status", "published"},
                 {"topic", "Kubernetes"},
                 {"tags", {"kubernetes", "talos"}}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k201Created);
    // topic + tags survive the create round-trip and serialize as expected.
    {
        auto created = json::parse(std::string(resp->body()))["data"];
        EXPECT_EQ(created.value("topic", ""), "Kubernetes");
        ASSERT_TRUE(created["tags"].is_array());
        EXPECT_EQ(created["tags"].size(), 2u);
        EXPECT_EQ(created["tags"][0], "kubernetes");
    }

    // Public list shows the published post as a lightweight card: it carries a
    // computed read_mins, omits the full body, and the envelope reports total.
    resp = call([&](auto cb) { controller.publicListPosts(TestHelpers::make_request(Get), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto list = json::parse(std::string(resp->body()));
    EXPECT_TRUE(list.contains("total"));
    EXPECT_GE(list.value("total", 0), 1);
    EXPECT_EQ(list.value("page", 0), 1);  // hybrid contract: 1-based page
    bool found = false;
    for (const auto& p : list["items"])
        if (p.value("slug", "") == "hello-test") {
            found = true;
            EXPECT_FALSE(p.contains("body"));       // cards never ship the body
            EXPECT_GE(p.value("read_mins", 0), 1);  // reading time computed server-side
        }
    EXPECT_TRUE(found);

    // Public get by slug.
    resp =
        call([&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "hello-test"); });
    EXPECT_EQ(resp->statusCode(), k200OK);

    // Unknown slug → 404.
    resp = call(
        [&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "does-not-exist"); });
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(PostsFlowTest, DraftHiddenFromPublic) {
    json body = {{"slug", "draft-test"}, {"title", "Draft"}, {"status", "draft"}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k201Created);
    // Drafts must never be exposed on the public read path.
    resp =
        call([&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "draft-test"); });
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(PostsFlowTest, PublicListFiltersAndFacets) {
    // Tests in this binary share one DB — every filterable value here is
    // unique to this test so earlier tests' posts can't leak into counts.
    seed("a-k8s", "Facet Kube A", "FacetK8s", {"facet-kube", "facet-talos"});
    seed("b-k8s", "Facet Kube B", "FacetK8s", {"facet-kube"});
    seed("c-sre", "Facet SLO Burnrate", "FacetSRE", {"facet-prom"});
    seed("d-other", "Facet Notes", "", json::array());

    auto req = TestHelpers::make_request(Get);
    req->setParameter("topic", "FacetK8s");
    req->setParameter("include", "facets");
    auto resp = call([&](auto cb) { controller.publicListPosts(req, std::move(cb)); });
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 2);
    EXPECT_EQ(body["items"].size(), 2u);
    EXPECT_EQ(body["page"], 1);
    for (auto& it : body["items"])
        EXPECT_FALSE(it.contains("body"));
    // Facets are computed over the CURRENT filter.
    EXPECT_EQ(body["facets"]["topics"][0]["name"], "FacetK8s");
    EXPECT_EQ(body["facets"]["topics"][0]["count"], 2);
    EXPECT_EQ(body["facets"]["tags"][0]["name"], "facet-kube");

    // tag filter
    auto req2 = TestHelpers::make_request(Get);
    req2->setParameter("tag", "facet-prom");
    auto r2 = call([&](auto cb) { controller.publicListPosts(req2, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r2->body()))["total"], 1);

    // q searches title+summary
    auto req3 = TestHelpers::make_request(Get);
    req3->setParameter("q", "burnrate");
    auto r3 = call([&](auto cb) { controller.publicListPosts(req3, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r3->body()))["total"], 1);

    // Empty topic groups as "Other" and is filterable as such. Other tests may
    // also have blank-topic posts, so assert containment, not an exact count.
    auto req4 = TestHelpers::make_request(Get);
    req4->setParameter("topic", "Other");
    req4->setParameter("limit", "50");
    auto r4 = call([&](auto cb) { controller.publicListPosts(req4, std::move(cb)); });
    auto b4 = json::parse(std::string(r4->body()));
    bool other_found = false;
    for (auto& it : b4["items"])
        if (it["slug"] == "d-other")
            other_found = true;
    EXPECT_TRUE(other_found);
    for (auto& it : b4["items"])
        EXPECT_TRUE(it["topic"].get<std::string>().empty() || it["topic"] == "Other");
}

TEST_F(PostsFlowTest, PublicGetIncludesAdjacent) {
    seed("p1", "First", "T", json::array());
    seed("p2", "Middle", "T", json::array());
    seed("p3", "Last", "T", json::array());
    auto req = TestHelpers::make_request(Get);
    req->setParameter("include", "adjacent");
    auto resp = call([&](auto cb) { controller.publicGetPost(req, std::move(cb), "p2"); });
    auto body = json::parse(std::string(resp->body()));
    // Newest-first feed: p3 is newer (next), p1 is older (prev).
    EXPECT_EQ(body["data"]["adjacent"]["next"]["slug"], "p3");
    EXPECT_EQ(body["data"]["adjacent"]["prev"]["slug"], "p1");

    auto reqEdge = TestHelpers::make_request(Get);
    reqEdge->setParameter("include", "adjacent");
    auto rEdge = call([&](auto cb) { controller.publicGetPost(reqEdge, std::move(cb), "p3"); });
    auto bEdge = json::parse(std::string(rEdge->body()));
    EXPECT_TRUE(bEdge["data"]["adjacent"]["next"].is_null());
    EXPECT_EQ(bEdge["data"]["adjacent"]["prev"]["slug"], "p2");
}

TEST_F(PostsFlowTest, PublicListClampsLimitTo50) {
    auto req = TestHelpers::make_request(Get);
    req->setParameter("limit", "1000");
    auto resp = call([&](auto cb) { controller.publicListPosts(req, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(resp->body()))["limit"], 50);
}

TEST_F(PostsFlowTest, LeetCodeTopicBackfill) {
    // Migrations 008+009 persist the old JS display rule: EVERY numeric-slug
    // post folds into topic "LeetCode" — 008 covered blank topics, 009 folds
    // category topics (Array/String/...) too, which the JS used to override.
    // A fresh test DB has already run both; re-run the 009 statement against
    // posts created afterwards and assert the fold semantics hold.
    json blank = {{"slug", "123-two-sum"}, {"title", "Two Sum"}, {"status", "published"}};
    auto r1 = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, blank), std::move(cb)); });
    ASSERT_EQ(r1->statusCode(), k201Created);
    json categorized = {{"slug", "124-max-path"}, {"title", "Max Path"}, {"status", "published"}, {"topic", "Tree"}};
    auto r2 =
        call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, categorized), std::move(cb)); });
    ASSERT_EQ(r2->statusCode(), k201Created);
    Database::get().execute_write([](auto& txn) {
        txn.exec("UPDATE posts SET topic = 'LeetCode' WHERE slug ~ '^\\d+-' AND topic <> 'LeetCode'");
        return 0;
    });
    Repositories::PostRepository repo;
    auto blank_found = repo.find_published_by_slug("123-two-sum");
    ASSERT_TRUE(blank_found.has_value());
    EXPECT_EQ(blank_found->topic, "LeetCode");
    auto cat_found = repo.find_published_by_slug("124-max-path");
    ASSERT_TRUE(cat_found.has_value());
    EXPECT_EQ(cat_found->topic, "LeetCode");
}

TEST_F(PostsFlowTest, DraftPreviewToken) {
    json body = {{"slug", "wip"}, {"title", "WIP"}, {"status", "draft"}};
    auto created = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(created->statusCode(), k201Created);
    const std::string id = json::parse(std::string(created->body()))["data"]["id"];

    // Draft is invisible without a token.
    auto r404 = call([&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "wip"); });
    EXPECT_EQ(r404->statusCode(), k404NotFound);

    // Issue a preview token.
    auto issued = call([&](auto cb) { controller.previewToken(TestHelpers::make_request(Post), std::move(cb), id); });
    ASSERT_EQ(issued->statusCode(), k200OK);
    auto ib = json::parse(std::string(issued->body()));
    const std::string url = ib["data"]["url"];
    ASSERT_NE(url.find("/blog/wip?preview="), std::string::npos);
    EXPECT_NE(ib["data"]["expires_at"].get<std::string>().find("T"), std::string::npos);
    const std::string tok = url.substr(url.find("preview=") + 8);

    // Valid token → draft served.
    auto reqTok = TestHelpers::make_request(Get);
    reqTok->setParameter("preview", tok);
    auto rOk = call([&](auto cb) { controller.publicGetPost(reqTok, std::move(cb), "wip"); });
    EXPECT_EQ(rOk->statusCode(), k200OK);

    // Token bound to a DIFFERENT post id → 404.
    const std::string other = Security::Tokens::issue(Security::Auth::get().config().jwt_secret,
                                                      "00000000-0000-0000-0000-000000000000",
                                                      Security::Tokens::Purpose::Preview,
                                                      std::chrono::seconds(3600));
    auto reqBad = TestHelpers::make_request(Get);
    reqBad->setParameter("preview", other);
    auto rBad = call([&](auto cb) { controller.publicGetPost(reqBad, std::move(cb), "wip"); });
    EXPECT_EQ(rBad->statusCode(), k404NotFound);

    // Expired token → 404.
    const std::string expired = Security::Tokens::issue(
        Security::Auth::get().config().jwt_secret, id, Security::Tokens::Purpose::Preview, std::chrono::seconds(-1));
    auto reqExp = TestHelpers::make_request(Get);
    reqExp->setParameter("preview", expired);
    auto rExp = call([&](auto cb) { controller.publicGetPost(reqExp, std::move(cb), "wip"); });
    EXPECT_EQ(rExp->statusCode(), k404NotFound);

    // Drafts never appear in the public list even while a preview token exists.
    auto rList = call([&](auto cb) { controller.publicListPosts(TestHelpers::make_request(Get), std::move(cb)); });
    for (auto& it : json::parse(std::string(rList->body()))["items"])
        EXPECT_NE(it["slug"], "wip");
}

TEST_F(PostsFlowTest, AdminListFilters) {
    json draft = {
        {"slug", "admfilter-draft"}, {"title", "AdmFilter SLO Draft"}, {"status", "draft"}, {"topic", "AdmFilterSRE"}};
    auto rc = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, draft), std::move(cb)); });
    ASSERT_EQ(rc->statusCode(), k201Created);
    seed("admfilter-pub", "AdmFilter Kube Pub", "AdmFilterK8s", {"admfilter-kube"});

    // status filter sees drafts.
    auto req = TestHelpers::make_request(Get);
    req->setParameter("status", "draft");
    req->setParameter("q", "admfilter");
    auto resp = call([&](auto cb) { controller.listPosts(req, std::move(cb)); });
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 1);
    EXPECT_EQ(body["data"][0]["slug"], "admfilter-draft");

    // q over title+slug+summary.
    auto req2 = TestHelpers::make_request(Get);
    req2->setParameter("q", "admfilter");
    auto r2 = call([&](auto cb) { controller.listPosts(req2, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(r2->body()))["total"], 2);

    // Bad status → 400.
    auto req3 = TestHelpers::make_request(Get);
    req3->setParameter("status", "nope");
    auto r3 = call([&](auto cb) { controller.listPosts(req3, std::move(cb)); });
    EXPECT_EQ(r3->statusCode(), k400BadRequest);
}

TEST_F(PostsFlowTest, CreateRejectsMissingFields) {
    json body = {{"summary", "no slug or title"}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(PostsFlowTest, DuplicateSlugReturns409NotHtml500) {
    json body = {{"slug", "dup-slug"}, {"title", "First"}, {"status", "draft"}};
    auto r1 = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(r1->statusCode(), k201Created);
    body["title"] = "Second";
    auto r2 = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(r2->statusCode(), k409Conflict);  // was a bare 500 before typed errors
}

TEST_F(PostsFlowTest, NonStringFieldIs400Not500) {
    // summary as a number used to throw type_error.306 out of the handler → 500.
    json body = {{"slug", "num-summary"}, {"title", "T"}, {"summary", 42}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(PostsFlowTest, RejectsUnsafeSlug) {
    for (const char* bad : {"has space", "Upper", "slash/here", "has#hash", "-lead", "trail-"}) {
        json body = {{"slug", bad}, {"title", "T"}};
        auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
        EXPECT_EQ(resp->statusCode(), k400BadRequest) << "slug: " << bad;
    }
    // A numeric LeetCode-style slug stays valid. (Unique to this test — the
    // suite shares one DB, so reusing another test's slug would 409.)
    json ok = {{"slug", "9999-slugcheck-ok"}, {"title", "T"}};
    auto r = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, ok), std::move(cb)); });
    EXPECT_EQ(r->statusCode(), k201Created);
}

TEST_F(PostsFlowTest, TagFilterEscapesLikeMetacharacters) {
    // A '%' in a tag must match literally, not act as a SQL LIKE wildcard.
    seed("pct-tag", "Pct", "T", {"c++", "50pct"});
    seed("other-tag", "Other", "T", {"golang"});
    auto req = TestHelpers::make_request(Get);
    req->setParameter("tag", "50%");  // no such tag; '%' must NOT wildcard-match "50pct"
    auto resp = call([&](auto cb) { controller.publicListPosts(req, std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(resp->body()))["total"], 0);
}

TEST_F(PostsFlowTest, CreateRejectsCommaInTag) {
    // A comma inside a tag would corrupt the comma-joined storage — reject it.
    json body = {{"slug", "bad-tag"}, {"title", "Bad tag"}, {"tags", {"a,b"}}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// ── SSR / SEO surface: fixture with a configured canonical base URL ─────────
class PublicPagesTest : public PostsFlowTest {
protected:
    Api::PublicPagesController pages;
    std::string config_file_name() const override { return "public_pages_test_config.json"; }
    void config_overrides(json& cfg) override {
        PostsFlowTest::config_overrides(cfg);
        cfg["site"]["base_url"] = "https://example.test";
    }
};

TEST_F(PublicPagesTest, SitemapUsesConfiguredBaseAndCaches) {
    seed("sm-post", "Sitemap Post", "T", json::array());
    auto resp = call([&](auto cb) { pages.sitemap(TestHelpers::make_request(Get), std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    const std::string body(resp->body());
    EXPECT_NE(body.find("<loc>https://example.test/</loc>"), std::string::npos);
    EXPECT_NE(body.find("<loc>https://example.test/blog/sm-post</loc>"), std::string::npos);
    // The only http:// allowed is the sitemap XML namespace itself.
    EXPECT_EQ(body.find("http://"), body.find("http://www.sitemaps.org"));
    EXPECT_EQ(resp->getHeader("cache-control"), "public, max-age=3600");
}

TEST_F(PublicPagesTest, BlogPostSsrHeadIsoDatesAndIsland) {
    json body = {{"slug", "ssr-post"},
                 {"title", "SSR & Co"},
                 {"summary", "sum"},
                 {"body", "# hello"},
                 {"status", "published"},
                 {"topic", "SRE"},
                 {"tags", {"sre"}}};
    auto created = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(created->statusCode(), k201Created);

    auto resp = call([&](auto cb) { pages.blogPost(TestHelpers::make_request(Get), std::move(cb), "ssr-post"); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    const std::string html(resp->body());
    EXPECT_NE(html.find("<title>SSR &amp; Co — Michael Tarassov</title>"), std::string::npos);
    EXPECT_NE(html.find("<link rel=\"canonical\" href=\"https://example.test/blog/ssr-post\">"), std::string::npos);
    EXPECT_NE(html.find("og:url\" content=\"https://example.test/blog/ssr-post\""), std::string::npos);
    EXPECT_NE(html.find("id=\"post-data\""), std::string::npos);
    // JSON-LD dates are ISO 8601 — the raw Postgres "+00" form must be gone.
    EXPECT_EQ(html.find("+00\""), std::string::npos);
    EXPECT_NE(html.find("\"datePublished\":\"2"), std::string::npos);
    // No noindex on published posts.
    EXPECT_EQ(html.find("noindex"), std::string::npos);
    // The page carries the PUBLIC-SITE CSP — the API middleware's
    // default-src 'none' would brick the page (browsers intersect CSPs).
    EXPECT_NE(resp->getHeader("content-security-policy").find("script-src 'self'"), std::string::npos);
    // Unknown slug → 404 (template-rendered), same CSP treatment.
    auto r404 = call([&](auto cb) { pages.blogPost(TestHelpers::make_request(Get), std::move(cb), "no-such-post"); });
    EXPECT_EQ(r404->statusCode(), k404NotFound);
    EXPECT_NE(r404->getHeader("content-security-policy").find("script-src 'self'"), std::string::npos);
}

TEST_F(PublicPagesTest, BlogPostDraftPreviewIsNoindexed) {
    json body = {{"slug", "wip-page"}, {"title", "WIP"}, {"status", "draft"}};
    auto created = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(created->statusCode(), k201Created);
    const std::string id = json::parse(std::string(created->body()))["data"]["id"];

    auto r404 = call([&](auto cb) { pages.blogPost(TestHelpers::make_request(Get), std::move(cb), "wip-page"); });
    EXPECT_EQ(r404->statusCode(), k404NotFound);

    const std::string tok = Security::Tokens::issue(
        Security::Auth::get().config().jwt_secret, id, Security::Tokens::Purpose::Preview, std::chrono::seconds(3600));
    auto req = TestHelpers::make_request(Get);
    req->setParameter("preview", tok);
    auto rOk = call([&](auto cb) { pages.blogPost(req, std::move(cb), "wip-page"); });
    ASSERT_EQ(rOk->statusCode(), k200OK);
    EXPECT_NE(std::string(rOk->body()).find("noindex"), std::string::npos);
}

TEST_F(PostsFlowTest, SsrSitemap) {
    // Publish a post, then exercise the server-rendered SEO surfaces.
    // (The legacy /blog-single.html redirect is gone — spec 2026-07-25.)
    json body = {{"slug", "ssr-test"},
                 {"title", "SSR Test"},
                 {"summary", "A concise summary."},
                 {"body", "# hi\n\nbody"},
                 {"status", "published"},
                 {"topic", "Kubernetes"},
                 {"tags", {"kubernetes"}}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k201Created);

    Api::PublicPagesController pub;

    // SSR post page: 200 HTML with a real per-post head + JSON-LD.
    resp = call([&](auto cb) { pub.blogPost(TestHelpers::make_request(Get), std::move(cb), "ssr-test"); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    std::string html(resp->body());
    EXPECT_NE(html.find("<title>SSR Test"), std::string::npos);
    EXPECT_NE(html.find("rel=\"canonical\""), std::string::npos);
    EXPECT_NE(html.find("/blog/ssr-test"), std::string::npos);
    EXPECT_NE(html.find("application/ld+json"), std::string::npos);
    EXPECT_NE(html.find("BlogPosting"), std::string::npos);

    // Unknown slug → 404.
    resp = call([&](auto cb) { pub.blogPost(TestHelpers::make_request(Get), std::move(cb), "no-such-post"); });
    EXPECT_EQ(resp->statusCode(), k404NotFound);

    // Sitemap lists the published post at its clean URL.
    resp = call([&](auto cb) { pub.sitemap(TestHelpers::make_request(Get), std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    std::string xml(resp->body());
    EXPECT_NE(xml.find("<urlset"), std::string::npos);
    EXPECT_NE(xml.find("/blog/ssr-test"), std::string::npos);
}

}  // namespace
