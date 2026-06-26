/**
 * @file ApiKey.hpp
 * @brief API key / personal access token row (api_keys table, migration 005).
 *        The secret and its hash NEVER appear in the DTO — only metadata.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Domain {

struct ApiKey {
    std::string id;
    std::string user_id;
    std::string name;
    std::string prefix;  // first chars of the key, e.g. "cpk_a1b2c3d4"
    std::optional<std::string> last_used_at;
    std::optional<std::string> revoked_at;
    std::string created_at;

    template <typename Row>
    static ApiKey from_row(const Row& row) {
        ApiKey k;
        k.id = row["id"].template as<std::string>();
        k.user_id = row["user_id"].template as<std::string>();
        k.name = row["name"].template as<std::string>();
        k.prefix = row["prefix"].template as<std::string>();
        if (!row["last_used_at"].is_null())
            k.last_used_at = row["last_used_at"].template as<std::string>();
        if (!row["revoked_at"].is_null())
            k.revoked_at = row["revoked_at"].template as<std::string>();
        k.created_at = row["created_at"].template as<std::string>();
        return k;
    }
};

inline void to_json(nlohmann::json& j, const ApiKey& k) {
    // key_hash is intentionally absent — the secret/hash never leaves the DB.
    j = nlohmann::json{{"id", k.id},
                       {"name", k.name},
                       {"prefix", k.prefix},
                       {"last_used_at", k.last_used_at ? nlohmann::json(*k.last_used_at) : nlohmann::json(nullptr)},
                       {"revoked_at", k.revoked_at ? nlohmann::json(*k.revoked_at) : nlohmann::json(nullptr)},
                       {"created_at", k.created_at}};
}

}  // namespace Domain
