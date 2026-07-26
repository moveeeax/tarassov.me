/**
 * @file test_uploads_admin.cpp
 * @brief Admin upload endpoints: POST /api/v1/admin/uploads (multipart, with
 *        the extension allowlist / size cap / magic-byte sniff), GET
 *        /api/v1/admin/uploads (paged listing) and DELETE
 *        /api/v1/admin/uploads/{name}. Runs against a temp-dir LocalStorage
 *        installed via the test seam.
 *
 * Note these call the controller object directly, so the sync advices (auth,
 * content-type gate, rate limiter) do NOT run — the wire-level counterpart is
 * HttpE2E.MultipartUploadPassesContentTypeGate.
 */

#include <cstddef>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/UploadController.hpp"
#include "storage/Storage.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

// Signature prefixes the sniff looks for (see Api::image_bytes_match).
// std::string(ptr, len) — the const char* ctor would stop at the PNG NUL.
const std::string kPngMagic("\x89PNG\r\n\x1a\n", 8);
const std::string kGifMagic("GIF89a", 6);

/**
 * @brief Build the multipart/form-data POST the admin editor's fetch() sends.
 * @details Same body layout as HttpE2E.MultipartUploadPassesContentTypeGate,
 *          which proves this shape parses. MultiPartParser reads the boundary
 *          off the request's content-type, so it is set both as a header (what
 *          the parser looks up) and via setContentTypeString.
 */
HttpRequestPtr upload_request(const std::string& filename,
                              const std::string& part_content_type,
                              const std::string& bytes) {
    const std::string boundary = "----uploadsAdminTestBoundary";
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    body += "Content-Type: " + part_content_type + "\r\n\r\n";
    body += bytes + "\r\n";
    body += "--" + boundary + "--\r\n";

    auto req = TestHelpers::make_request(Post);
    req->setBody(body);
    req->addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    req->setContentTypeString("multipart/form-data; boundary=" + boundary);
    return req;
}

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

    HttpResponsePtr do_upload(const std::string& filename,
                              const std::string& part_content_type,
                              const std::string& bytes) {
        auto req = upload_request(filename, part_content_type, bytes);
        return call([&](auto cb) { controller.upload(req, std::move(cb)); });
    }

    /// Number of objects currently under the posts/ prefix (seeded with 2).
    std::size_t stored_count() { return Storage::get().list("posts/").size(); }
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

// ---------------------------------------------------------------------------
// POST /api/v1/admin/uploads — the validation chain, one test per rejection
// reason. Each also asserts nothing was written: a rejection that still stores
// the file would be worse than no check at all.
// ---------------------------------------------------------------------------

TEST_F(UploadsAdminTest, UploadStoresAValidPngUnderThePostsPrefix) {
    auto resp = do_upload("pic.png", "image/png", kPngMagic + "payload bytes");
    ASSERT_EQ(resp->statusCode(), k201Created) << resp->body();
    auto body = json::parse(std::string(resp->body()));
    const auto key = body["data"]["key"].get<std::string>();
    EXPECT_EQ(key.rfind("posts/", 0), 0u) << key;
    EXPECT_EQ(key.substr(key.size() - 4), ".png") << key;
    // The client-supplied filename is never reused as the key.
    EXPECT_EQ(key.find("pic"), std::string::npos) << key;
    // Whatever base Storage::url() is configured with, the returned URL has to
    // address the key we just stored.
    const auto url = body["data"]["url"].get<std::string>();
    EXPECT_NE(url.find(key), std::string::npos) << url;
    EXPECT_TRUE(Storage::get().exists(key));
    EXPECT_EQ(stored_count(), 3u);
}

TEST_F(UploadsAdminTest, UploadRejectsSvgAndOtherNonRasterExtensions) {
    // SVG is the one that matters: it can carry inline <script>, and `nosniff`
    // does not neutralize it when served as image/svg+xml from our own origin.
    // It must be rejected on the extension, before the sniff ever runs.
    const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>)";
    auto svg_resp = do_upload("logo.svg", "image/svg+xml", svg);
    ASSERT_EQ(svg_resp->statusCode(), k400BadRequest) << svg_resp->body();
    EXPECT_EQ(json::parse(std::string(svg_resp->body()))["error"].get<std::string>(), "unsupported_type");

    for (const char* name : {"note.txt", "page.html", "payload.bmp", "noextension"}) {
        auto resp = do_upload(name, "application/octet-stream", kPngMagic + "payload");
        EXPECT_EQ(resp->statusCode(), k400BadRequest) << name;
        EXPECT_EQ(json::parse(std::string(resp->body()))["error"].get<std::string>(), "unsupported_type") << name;
    }
    EXPECT_EQ(stored_count(), 2u) << "a rejected upload was stored anyway";
}

TEST_F(UploadsAdminTest, UploadRejectsContentThatDoesNotMatchTheExtension) {
    // A .png whose bytes are markup — the stored-XSS case the sniff exists for.
    const std::string html = "<!DOCTYPE html><html><body><script>alert(1)</script></body></html>";
    auto resp = do_upload("pic.png", "image/png", html);
    ASSERT_EQ(resp->statusCode(), k400BadRequest) << resp->body();
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"].get<std::string>(), "bad_content");

    // Allowed extension, allowed-but-different magic: a GIF renamed .png.
    auto mismatched = do_upload("pic.png", "image/png", kGifMagic + "payload");
    ASSERT_EQ(mismatched->statusCode(), k400BadRequest) << mismatched->body();
    EXPECT_EQ(json::parse(std::string(mismatched->body()))["error"].get<std::string>(), "bad_content");

    // Truncated signature: the right bytes, not enough of them.
    auto truncated = do_upload("pic.png", "image/png", kPngMagic.substr(0, 4));
    ASSERT_EQ(truncated->statusCode(), k400BadRequest) << truncated->body();
    EXPECT_EQ(json::parse(std::string(truncated->body()))["error"].get<std::string>(), "bad_content");

    EXPECT_EQ(stored_count(), 2u) << "a rejected upload was stored anyway";
}

TEST_F(UploadsAdminTest, UploadRejectsFilesOverTheSizeCap) {
    // 5 MB + 1 of otherwise-valid PNG: the cap is checked before the sniff.
    constexpr std::size_t kMaxBytes = 5 * 1024 * 1024;
    std::string oversized = kPngMagic;
    oversized.append(kMaxBytes + 1 - kPngMagic.size(), 'x');
    ASSERT_EQ(oversized.size(), kMaxBytes + 1);

    auto resp = do_upload("big.png", "image/png", oversized);
    ASSERT_EQ(resp->statusCode(), k400BadRequest) << resp->body();
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"].get<std::string>(), "bad_size");
    EXPECT_EQ(stored_count(), 2u) << "an oversized upload was stored anyway";
}

TEST_F(UploadsAdminTest, UploadRejectsARequestWithNoFilePart) {
    auto req = TestHelpers::make_request(Post);
    req->setBody("not multipart at all");
    auto resp = call([&](auto cb) { controller.upload(req, std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k400BadRequest) << resp->body();
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"].get<std::string>(), "no_file");
    EXPECT_EQ(stored_count(), 2u);
}

}  // namespace
