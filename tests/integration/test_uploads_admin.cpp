/**
 * @file test_uploads_admin.cpp
 * @brief Media library endpoints: GET /api/v1/admin/uploads (paged listing)
 *        and DELETE /api/v1/admin/uploads/{name}. Runs against a temp-dir
 *        LocalStorage installed via the test seam.
 */

#include <filesystem>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/UploadController.hpp"
#include "storage/Storage.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

class UploadsAdminTest : public TestHelpers::CoreBackedTest {
protected:
    Api::UploadController controller;
    std::filesystem::path root_ = std::filesystem::temp_directory_path() / "uploads-admin-test";
    std::string config_file_name() const override { return "uploads_admin_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        std::filesystem::remove_all(root_);
        Storage::install_for_testing(std::make_unique<Storage::LocalStorage>(root_, "http://cdn.test"));
        Storage::get().put("posts/media-a.png", "aaaa", "image/png");
        Storage::get().put("posts/media-b.jpg", "bb", "image/jpeg");
    }

    void TearDown() override {
        Storage::reset_for_testing();
        std::filesystem::remove_all(root_);
        TestHelpers::CoreBackedTest::TearDown();
    }

    template <typename Fn>
    HttpResponsePtr call(Fn&& fn) {
        HttpResponsePtr resp;
        fn([&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }
};

TEST_F(UploadsAdminTest, ListDeleteRoundTrip) {
    // List: admin envelope with name/url/content_type/created_at.
    auto resp = call([&](auto cb) { controller.listUploads(TestHelpers::make_request(Get), std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 2);
    ASSERT_EQ(body["data"].size(), 2u);
    const auto& first = body["data"][0];
    EXPECT_EQ(first["key"].get<std::string>().rfind("posts/", 0), 0u);
    EXPECT_NE(first["url"].get<std::string>().find("http://cdn.test/posts/"), std::string::npos);
    EXPECT_NE(first["content_type"].get<std::string>().find("image/"), std::string::npos);
    EXPECT_NE(first["created_at"].get<std::string>().find('T'), std::string::npos);

    // Delete by basename → gone; second delete → 404.
    auto rDel = call(
        [&](auto cb) { controller.deleteUpload(TestHelpers::make_request(Delete), std::move(cb), "media-a.png"); });
    EXPECT_EQ(rDel->statusCode(), k200OK);
    auto rList2 = call([&](auto cb) { controller.listUploads(TestHelpers::make_request(Get), std::move(cb)); });
    EXPECT_EQ(json::parse(std::string(rList2->body()))["total"], 1);
    auto rDel2 = call(
        [&](auto cb) { controller.deleteUpload(TestHelpers::make_request(Delete), std::move(cb), "media-a.png"); });
    EXPECT_EQ(rDel2->statusCode(), k404NotFound);

    // Traversal-shaped names are rejected before touching storage.
    auto rBad =
        call([&](auto cb) { controller.deleteUpload(TestHelpers::make_request(Delete), std::move(cb), "..evil"); });
    EXPECT_EQ(rBad->statusCode(), k400BadRequest);
}

}  // namespace
