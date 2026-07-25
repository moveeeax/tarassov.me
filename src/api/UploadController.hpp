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
#include <initializer_list>
#include <string>
#include <unordered_map>

#include <drogon/HttpController.h>
#include <drogon/MultiPart.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "storage/Storage.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

// Cheap magic-number sniff: confirm the bytes actually match the claimed image
// type, so a .jpg that's really HTML/script can't be stored and served back.
inline bool image_bytes_match(const std::string& ext, const std::string& b) {
    const auto starts = [&](std::initializer_list<unsigned char> sig) {
        if (b.size() < sig.size())
            return false;
        std::size_t i = 0;
        for (unsigned char c : sig)
            if (static_cast<unsigned char>(b[i++]) != c)
                return false;
        return true;
    };
    if (ext == "jpg" || ext == "jpeg")
        return starts({0xFF, 0xD8, 0xFF});
    if (ext == "png")
        return starts({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    if (ext == "gif")
        return b.size() >= 6 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0);
    if (ext == "webp")
        return b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0;
    return false;
}

class UploadController : public HttpController<UploadController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UploadController::upload, "/api/v1/admin/uploads", Post);
    // Media library: list + delete what the editor uploaded.
    ADD_METHOD_TO(UploadController::listUploads, "/api/v1/admin/uploads", Get);
    ADD_METHOD_TO(UploadController::deleteUpload, "/api/v1/admin/uploads/{1}", Delete);
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
        if (!image_bytes_match(ext, bytes)) {
            callback(ErrorResponse::bad_request("bad_content", "File content does not match its image type"));
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

    // GET /api/v1/admin/uploads — offset-paged listing of the posts/ prefix.
    void listUploads(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Storage backend not configured"));
            return;
        }
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        auto all = Storage::get().list("posts/");
        json data = json::array();
        const std::size_t from = std::min<std::size_t>(static_cast<std::size_t>(page.offset), all.size());
        const std::size_t to = std::min<std::size_t>(from + static_cast<std::size_t>(page.limit), all.size());
        for (std::size_t i = from; i < to; ++i) {
            const auto& o = all[i];
            const std::string name = o.key.substr(o.key.rfind('/') + 1);
            data.push_back({{"key", o.key},
                            {"name", name},
                            {"url", Storage::get().url(o.key)},
                            {"size_bytes", o.size_bytes},
                            {"content_type", content_type_for(name)},
                            {"created_at", o.last_modified}});
        }
        callback(Response::ok({{"data", data},
                               {"total", static_cast<long>(all.size())},
                               {"limit", page.limit},
                               {"offset", page.offset}}));
    }

    // DELETE /api/v1/admin/uploads/{name} — name is the single-segment
    // basename of an upload key (keys are posts/<hex>.<ext>).
    void deleteUpload(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& name) {
        API_REQUIRE_ADMIN(req, callback);
        if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos ||
            name.find('\\') != std::string::npos) {
            callback(ErrorResponse::bad_request("invalid_name", "Expected a single-segment object name"));
            return;
        }
        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Storage backend not configured"));
            return;
        }
        const std::string key = "posts/" + name;
        if (!Storage::get().exists(key)) {
            callback(ErrorResponse::not_found("upload"));
            return;
        }
        Storage::get().remove(key);
        callback(Response::ok({{"message", "Upload deleted"}}));
    }

private:
    // Extension → content type for listings (uploads are raster images only).
    static std::string content_type_for(const std::string& name) {
        const auto dot = name.rfind('.');
        std::string ext = dot == std::string::npos ? std::string{} : name.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == "jpg" || ext == "jpeg")
            return "image/jpeg";
        if (ext == "png")
            return "image/png";
        if (ext == "gif")
            return "image/gif";
        if (ext == "webp")
            return "image/webp";
        return "application/octet-stream";
    }
};

}  // namespace Api
