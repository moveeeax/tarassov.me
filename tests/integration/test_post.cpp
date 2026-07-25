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
    }

    // Helper: call a (req, cb) handler and return the response.
    template <typename Fn>
    HttpResponsePtr call(Fn&& fn) {
        HttpResponsePtr resp;
        fn([&](const HttpResponsePtr& r) { resp = r; });
        return resp;
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
    bool found = false;
    for (const auto& p : list["data"])
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

TEST_F(PostsFlowTest, CreateRejectsMissingFields) {
    json body = {{"summary", "no slug or title"}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(PostsFlowTest, CreateRejectsCommaInTag) {
    // A comma inside a tag would corrupt the comma-joined storage — reject it.
    json body = {{"slug", "bad-tag"}, {"title", "Bad tag"}, {"tags", {"a,b"}}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(PostsFlowTest, SsrSitemapAndRedirect) {
    // Publish a post, then exercise the server-rendered SEO surfaces.
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

    // Legacy ?slug= → 301 to the clean URL.
    auto req = TestHelpers::make_request(Get);
    req->setParameter("slug", "ssr-test");
    resp = call([&](auto cb) { pub.blogSingleRedirect(req, std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k301MovedPermanently);
    EXPECT_EQ(resp->getHeader("location"), "/blog/ssr-test");
}

}  // namespace
