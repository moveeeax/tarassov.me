/**
 * @file test_upload_validation.cpp
 * @brief Magic-byte sniff behind POST /api/v1/admin/uploads
 *        (`Api::image_bytes_match`). Pure function, no infra.
 *
 * This is the guard that stops a .png whose bytes are really HTML/SVG/script
 * from being stored and served back same-origin. It had no coverage at all,
 * so adding `{"svg", "image/svg+xml"}` to the extension allowlist or inverting
 * the sniff would have shipped green.
 */

#include <string>

#include <gtest/gtest.h>

#include "api/UploadController.hpp"

namespace {

// Signature prefixes, spelled out so a change to the sniff has to change a
// literal here too. std::string(ptr, len) — NOT the const char* ctor, which
// would stop at the embedded NUL in the PNG signature.
const std::string kPng("\x89PNG\r\n\x1a\n", 8);
const std::string kJpeg("\xFF\xD8\xFF", 3);
const std::string kGif87("GIF87a", 6);
const std::string kGif89("GIF89a", 6);
// RIFF + 4 little-endian size bytes + WEBP.
const std::string kWebp = std::string("RIFF", 4) + std::string("\x24\x00\x00\x00", 4) + std::string("WEBP", 4);

const std::string kPayload = "....trailing image payload....";

TEST(UploadImageSniff, AcceptsEachAllowedExtension) {
    EXPECT_TRUE(Api::image_bytes_match("png", kPng + kPayload));
    EXPECT_TRUE(Api::image_bytes_match("jpg", kJpeg + kPayload));
    EXPECT_TRUE(Api::image_bytes_match("jpeg", kJpeg + kPayload));
    EXPECT_TRUE(Api::image_bytes_match("gif", kGif87 + kPayload));
    EXPECT_TRUE(Api::image_bytes_match("gif", kGif89 + kPayload));
    EXPECT_TRUE(Api::image_bytes_match("webp", kWebp + kPayload));
}

TEST(UploadImageSniff, AcceptsTheSignatureWithNoPayload) {
    // Exactly-signature-length buffers are the boundary case of the size guards.
    EXPECT_TRUE(Api::image_bytes_match("png", kPng));
    EXPECT_TRUE(Api::image_bytes_match("jpg", kJpeg));
    EXPECT_TRUE(Api::image_bytes_match("gif", kGif89));
    EXPECT_TRUE(Api::image_bytes_match("webp", kWebp));
}

TEST(UploadImageSniff, RejectsBytesOfADifferentImageType) {
    // The claimed extension must match the actual bytes — a PNG renamed .jpg
    // is still a lie about the content type we will serve it with.
    EXPECT_FALSE(Api::image_bytes_match("jpg", kPng + kPayload));
    EXPECT_FALSE(Api::image_bytes_match("png", kJpeg + kPayload));
    EXPECT_FALSE(Api::image_bytes_match("gif", kPng + kPayload));
    EXPECT_FALSE(Api::image_bytes_match("webp", kGif89 + kPayload));
}

TEST(UploadImageSniff, RejectsMarkupAndScriptDisguisedAsAnImage) {
    // The whole point of the sniff: stored-XSS payloads carrying an image
    // extension.
    const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>)";
    const std::string html = "<!DOCTYPE html><html><body><script>alert(1)</script></body></html>";
    for (const char* ext : {"png", "jpg", "jpeg", "gif", "webp"}) {
        EXPECT_FALSE(Api::image_bytes_match(ext, svg)) << "ext=" << ext;
        EXPECT_FALSE(Api::image_bytes_match(ext, html)) << "ext=" << ext;
    }
}

TEST(UploadImageSniff, RejectsUnknownAndDangerousExtensions) {
    // "svg" is deliberately absent from the allowlist AND has no sniff branch,
    // so even a genuine SVG body cannot pass.
    EXPECT_FALSE(Api::image_bytes_match("svg", "<svg></svg>"));
    EXPECT_FALSE(Api::image_bytes_match("svgz", kPng));
    EXPECT_FALSE(Api::image_bytes_match("html", kPng));
    EXPECT_FALSE(Api::image_bytes_match("", kPng));
    // The controller lower-cases the extension before calling this; an
    // upper-case one must NOT match, or that normalization becomes optional.
    EXPECT_FALSE(Api::image_bytes_match("PNG", kPng));
}

TEST(UploadImageSniff, RejectsEmptyAndTruncatedBuffers) {
    // Short buffers must return false, never read past the end.
    for (const char* ext : {"png", "jpg", "jpeg", "gif", "webp"})
        EXPECT_FALSE(Api::image_bytes_match(ext, "")) << "ext=" << ext;

    EXPECT_FALSE(Api::image_bytes_match("png", kPng.substr(0, 7)));
    EXPECT_FALSE(Api::image_bytes_match("jpg", kJpeg.substr(0, 2)));
    EXPECT_FALSE(Api::image_bytes_match("gif", kGif89.substr(0, 5)));
    // WEBP needs 12 bytes: "RIFF" + size + "WEBP". 11 must not read b[8..11].
    EXPECT_FALSE(Api::image_bytes_match("webp", kWebp.substr(0, 11)));
    // Right length, right RIFF header, wrong form tag.
    EXPECT_FALSE(Api::image_bytes_match(
        "webp", std::string("RIFF", 4) + std::string("\x24\x00\x00\x00", 4) + std::string("AVI ", 4)));
}

}  // namespace
