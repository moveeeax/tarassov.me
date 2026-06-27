/**
 * @file Storage.hpp
 * @brief Object/file storage seam. A small backend interface plus a local-disk
 *        implementation, behind a global accessor (mirrors Cache/Email).
 *
 * The interface is the point: a fork swaps LocalStorage for an S3/GCS backend by
 * subclassing StorageBackend and installing it — call sites (an upload
 * controller, a job) don't change. Keys are opaque strings the caller chooses
 * (use a UUID, not a user-supplied filename); LocalStorage refuses traversal.
 *
 * Wiring an HTTP upload/download surface (multipart parse, a files metadata
 * table, owner-scoping) is app-specific — see docs/EXAMPLES or mirror the
 * api_keys controller. This layer is just durable get/put/remove.
 */

#pragma once

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "utils/Config.hpp"
#include "utils/Crypto.hpp"

namespace Storage {

/// Backend contract. All methods throw std::runtime_error on an unexpected I/O
/// failure; get()/exists() return empty/false for a simply-absent object.
class StorageBackend {
public:
    virtual ~StorageBackend() = default;
    virtual void put(const std::string& key, const std::string& bytes, const std::string& content_type) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool remove(const std::string& key) = 0;
    virtual bool exists(const std::string& key) = 0;
    /// A URL/locator a client can use to fetch the object (backend-specific:
    /// a public base + key for local/CDN, or a presigned URL for S3).
    virtual std::string url(const std::string& key) = 0;
};

/// Reject keys that could escape the storage root (path traversal / absolute
/// paths). Keys are meant to be opaque ids; this is defense-in-depth.
inline bool key_is_safe(const std::string& key) {
    if (key.empty() || key.front() == '/' || key.front() == '\\')
        return false;
    if (key.find("..") != std::string::npos)
        return false;
    if (key.find('\0') != std::string::npos)
        return false;
    return true;
}

/// Local-filesystem backend. Stores each object as a file under `root`.
class LocalStorage : public StorageBackend {
public:
    LocalStorage(std::filesystem::path root, std::string public_base)
        : root_(std::move(root)), public_base_(std::move(public_base)) {}

    void put(const std::string& key, const std::string& bytes, const std::string& /*content_type*/) override {
        const auto path = resolve(key);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("storage: cannot write " + key);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            throw std::runtime_error("storage: write failed for " + key);
    }

    std::optional<std::string> get(const std::string& key) override {
        const auto path = resolve(key);
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool remove(const std::string& key) override {
        std::error_code ec;
        return std::filesystem::remove(resolve(key), ec);
    }

    bool exists(const std::string& key) override {
        std::error_code ec;
        return std::filesystem::exists(resolve(key), ec);
    }

    std::string url(const std::string& key) override {
        if (!key_is_safe(key))
            throw std::runtime_error("storage: unsafe key");
        return public_base_.empty() ? key : public_base_ + "/" + key;
    }

private:
    std::filesystem::path resolve(const std::string& key) const {
        if (!key_is_safe(key))
            throw std::runtime_error("storage: unsafe key '" + key + "'");
        return root_ / key;
    }

    std::filesystem::path root_;
    std::string public_base_;
};

/// S3-compatible backend (MinIO, AWS S3, Cloudflare R2, …) over libcurl with
/// hand-rolled AWS Signature V4. Path-style addressing by default (what MinIO
/// and most self-hosted gateways want). Unlike LocalStorage this survives pod
/// restarts and is shared across replicas — the right choice for k8s.
class S3Storage : public StorageBackend {
public:
    struct Config {
        std::string endpoint;  // e.g. http://minio:9000 (scheme required)
        std::string region;    // e.g. us-east-1 (MinIO default)
        std::string bucket;
        std::string access_key;
        std::string secret_key;
        std::string public_base;  // public URL prefix for url(); empty → endpoint/bucket
        long timeout_sec = 30;
    };

    explicit S3Storage(Config cfg) : cfg_(std::move(cfg)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);  // idempotent; Mailer may have run it
        host_ = host_from_endpoint(cfg_.endpoint);
        if (cfg_.region.empty())
            cfg_.region = "us-east-1";
    }

    void put(const std::string& key, const std::string& bytes, const std::string& content_type) override {
        const long code = request("PUT", key, bytes, content_type, nullptr);
        if (code < 200 || code >= 300)
            throw std::runtime_error("s3: PUT " + key + " failed with HTTP " + std::to_string(code));
    }

    std::optional<std::string> get(const std::string& key) override {
        std::string body;
        const long code = request("GET", key, "", "", &body);
        if (code == 200)
            return body;
        return std::nullopt;
    }

    bool remove(const std::string& key) override {
        const long code = request("DELETE", key, "", "", nullptr);
        return code == 204 || code == 200;
    }

    bool exists(const std::string& key) override { return request("HEAD", key, "", "", nullptr) == 200; }

    std::string url(const std::string& key) override {
        if (!key_is_safe(key))
            throw std::runtime_error("storage: unsafe key");
        if (!cfg_.public_base.empty())
            return cfg_.public_base + "/" + key;
        return cfg_.endpoint + "/" + cfg_.bucket + "/" + key;
    }

private:
    // Read callback feeding the request body to libcurl during a PUT upload.
    struct ReadCtx {
        const std::string* data;
        std::size_t offset = 0;
    };
    static std::size_t read_cb(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
        auto* ctx = static_cast<ReadCtx*>(userdata);
        const std::size_t want = size * nitems;
        const std::size_t left = ctx->data->size() - ctx->offset;
        const std::size_t n = left < want ? left : want;
        if (n > 0) {
            std::memcpy(buffer, ctx->data->data() + ctx->offset, n);
            ctx->offset += n;
        }
        return n;
    }
    static std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
        auto* out = static_cast<std::string*>(userdata);
        if (out)
            out->append(ptr, size * nmemb);
        return size * nmemb;
    }

    static std::string host_from_endpoint(const std::string& ep) {
        auto pos = ep.find("://");
        std::string rest = pos == std::string::npos ? ep : ep.substr(pos + 3);
        const auto slash = rest.find('/');
        return slash == std::string::npos ? rest : rest.substr(0, slash);
    }

    // RFC 3986 encode a path, leaving unreserved chars and '/' (segment seps).
    static std::string uri_encode_path(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~' || c == '/')
                out.push_back(static_cast<char>(c));
            else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    }

    static void amz_dates(std::string& amzdate, std::string& datestamp) {
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char d[32];
        std::strftime(d, sizeof(d), "%Y%m%dT%H%M%SZ", &tm);
        amzdate = d;
        std::strftime(d, sizeof(d), "%Y%m%d", &tm);
        datestamp = d;
    }

    long request(const std::string& method,
                 const std::string& key,
                 const std::string& body,
                 const std::string& content_type,
                 std::string* out) {
        if (!key_is_safe(key))
            throw std::runtime_error("storage: unsafe key '" + key + "'");

        std::string amzdate, datestamp;
        amz_dates(amzdate, datestamp);

        const std::string payload_hash = Utils::Crypto::sha256_hex(body);
        const std::string canonical_uri = "/" + cfg_.bucket + "/" + uri_encode_path(key);
        const std::string canonical_headers =
            "host:" + host_ + "\n" + "x-amz-content-sha256:" + payload_hash + "\n" + "x-amz-date:" + amzdate + "\n";
        const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
        const std::string canonical_request = method + "\n" + canonical_uri + "\n" + "" + "\n" + canonical_headers +
                                              "\n" + signed_headers + "\n" + payload_hash;

        const std::string scope = datestamp + "/" + cfg_.region + "/s3/aws4_request";
        const std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" + amzdate + "\n" + scope + "\n" + Utils::Crypto::sha256_hex(canonical_request);

        const std::string k_date = Utils::Crypto::hmac_sha256("AWS4" + cfg_.secret_key, datestamp);
        const std::string k_region = Utils::Crypto::hmac_sha256(k_date, cfg_.region);
        const std::string k_service = Utils::Crypto::hmac_sha256(k_region, "s3");
        const std::string k_signing = Utils::Crypto::hmac_sha256(k_service, "aws4_request");
        const std::string sig_bytes = Utils::Crypto::hmac_sha256(k_signing, string_to_sign);
        const std::string signature = Utils::Crypto::detail::bytes_to_hex(
            reinterpret_cast<const unsigned char*>(sig_bytes.data()), sig_bytes.size());

        const std::string authorization = "AWS4-HMAC-SHA256 Credential=" + cfg_.access_key + "/" + scope +
                                          ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

        CURL* h = curl_easy_init();
        if (!h)
            throw std::runtime_error("s3: curl_easy_init failed");

        const std::string full_url = cfg_.endpoint + canonical_uri;
        curl_easy_setopt(h, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(h, CURLOPT_TIMEOUT, cfg_.timeout_sec);

        ReadCtx rctx{&body, 0};
        if (method == "PUT") {
            curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(h, CURLOPT_READFUNCTION, &read_cb);
            curl_easy_setopt(h, CURLOPT_READDATA, &rctx);
            curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(body.size()));
        } else if (method == "HEAD") {
            curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        } else if (method != "GET") {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, out);

        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, ("Host: " + host_).c_str());
        hdrs = curl_slist_append(hdrs, ("x-amz-date: " + amzdate).c_str());
        hdrs = curl_slist_append(hdrs, ("x-amz-content-sha256: " + payload_hash).c_str());
        hdrs = curl_slist_append(hdrs, ("Authorization: " + authorization).c_str());
        if (method == "PUT" && !content_type.empty())
            hdrs = curl_slist_append(hdrs, ("Content-Type: " + content_type).c_str());
        // libcurl adds "Expect: 100-continue" on PUT; strip it (some gateways stall).
        hdrs = curl_slist_append(hdrs, "Expect:");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

        const CURLcode rc = curl_easy_perform(h);
        long code = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(h);

        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("s3: ") + method + " transport error: " + curl_easy_strerror(rc));
        return code;
    }

    Config cfg_;
    std::string host_;
};

// ── Global accessor (mirrors Cache/Email) ────────────────────────────────────
inline std::unique_ptr<StorageBackend> global_storage = nullptr;

inline bool is_initialized() {
    return global_storage != nullptr;
}

inline StorageBackend& get() {
    if (!global_storage)
        throw std::runtime_error("Storage not initialized");
    return *global_storage;
}

/// Bring up the configured backend. Only "local" ships; a fork adds others.
inline void initialize(Config::AppConfig& cfg) {
    const std::string backend = cfg.get<std::string>("storage.backend", "STORAGE_BACKEND", "local");
    if (backend == "local") {
        const std::string root = cfg.get<std::string>("storage.local.root", "STORAGE_LOCAL_ROOT", "data/uploads");
        const std::string base = cfg.get<std::string>("storage.public_base_url", "STORAGE_PUBLIC_BASE_URL", "");
        global_storage = std::make_unique<LocalStorage>(std::filesystem::path(root), base);
        spdlog::info("Storage: local backend at '{}'", root);
    } else if (backend == "s3") {
        S3Storage::Config s3;
        s3.endpoint = cfg.get<std::string>("storage.s3.endpoint", "S3_ENDPOINT", "");
        s3.region = cfg.get<std::string>("storage.s3.region", "S3_REGION", "us-east-1");
        s3.bucket = cfg.get<std::string>("storage.s3.bucket", "S3_BUCKET", "");
        s3.access_key = cfg.get<std::string>("storage.s3.access_key", "S3_ACCESS_KEY", "");
        s3.secret_key = cfg.get<std::string>("storage.s3.secret_key", "S3_SECRET_KEY", "");
        s3.public_base = cfg.get<std::string>("storage.public_base_url", "STORAGE_PUBLIC_BASE_URL", "");
        if (s3.endpoint.empty() || s3.bucket.empty())
            throw std::runtime_error("storage.backend=s3 requires S3_ENDPOINT and S3_BUCKET");
        global_storage = std::make_unique<S3Storage>(std::move(s3));
        spdlog::info("Storage: s3 backend at '{}' bucket '{}'",
                     cfg.get<std::string>("storage.s3.endpoint", "S3_ENDPOINT", ""),
                     cfg.get<std::string>("storage.s3.bucket", "S3_BUCKET", ""));
    } else {
        throw std::runtime_error("storage.backend '" + backend +
                                 "' is not built in — install a StorageBackend for it (see Storage.hpp)");
    }
}

// Test seam — swap in a fake/temp backend.
inline void install_for_testing(std::unique_ptr<StorageBackend> backend) {
    global_storage = std::move(backend);
}
inline void reset_for_testing() {
    global_storage.reset();
}

}  // namespace Storage
