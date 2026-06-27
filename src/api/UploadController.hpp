/**
 * @file UploadController.hpp
 * @brief Admin image upload: POST /api/v1/admin/uploads (multipart) stores the
 *        file in the configured Storage backend (local or S3/MinIO) under a
 *        random key and returns its public URL. The admin post editor inserts
 *        that URL into the Markdown body as an image.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_map>

#include <drogon/HttpController.h>
#include <drogon/MultiPart.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "storage/Storage.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class UploadController : public HttpController<UploadController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UploadController::upload, "/api/v1/admin/uploads", Post);
    METHOD_LIST_END

    void upload(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);

        MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().empty()) {
            callback(ErrorResponse::bad_request("no_file", "Expected a multipart file upload"));
            return;
        }
        const auto& file = parser.getFiles()[0];

        std::string ext(file.getFileExtension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        static const std::unordered_map<std::string, std::string> kTypes = {
            {"jpg", "image/jpeg"},
            {"jpeg", "image/jpeg"},
            {"png", "image/png"},
            {"gif", "image/gif"},
            {"webp", "image/webp"},
            // SVG intentionally excluded: it can carry inline <script> → stored
            // XSS when served same-origin. Raster only.
        };
        const auto type_it = kTypes.find(ext);
        if (type_it == kTypes.end()) {
            callback(ErrorResponse::bad_request("unsupported_type", "Allowed: jpg, jpeg, png, gif, webp"));
            return;
        }

        const std::string bytes(file.fileContent());
        constexpr std::size_t kMaxBytes = 5 * 1024 * 1024;  // 5 MB
        if (bytes.empty() || bytes.size() > kMaxBytes) {
            callback(ErrorResponse::bad_request("bad_size", "File must be 1 byte – 5 MB"));
            return;
        }

        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Storage backend not configured"));
            return;
        }

        // Opaque random key (never a client-supplied filename) under a posts/ prefix.
        const std::string key = "posts/" + Utils::Crypto::random_hex(16) + "." + ext;
        try {
            Storage::get().put(key, bytes, type_it->second);
        } catch (const std::exception& e) {
            spdlog::error("upload: storage put failed for {}: {}", key, e.what());
            callback(ErrorResponse::service_unavailable("storage_error", "Could not store the file"));
            return;
        }

        callback(Response::created({{"data", {{"key", key}, {"url", Storage::get().url(key)}}}}));
    }
};

}  // namespace Api
