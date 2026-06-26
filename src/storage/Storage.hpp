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

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include "utils/Config.hpp"

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
