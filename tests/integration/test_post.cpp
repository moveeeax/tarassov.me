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
    json body = {
        {"slug", "hello-test"}, {"title", "Hello"}, {"summary", "s"}, {"body", "# h"}, {"status", "published"}};
    auto resp = call([&](auto cb) { controller.createPost(TestHelpers::make_request(Post, body), std::move(cb)); });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k201Created);

    // Public list shows the published post.
    resp = call([&](auto cb) { controller.publicListPosts(TestHelpers::make_request(Get), std::move(cb)); });
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto list = json::parse(std::string(resp->body()));
    bool found = false;
    for (const auto& p : list["data"])
        if (p.value("slug", "") == "hello-test")
            found = true;
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

}  // namespace
