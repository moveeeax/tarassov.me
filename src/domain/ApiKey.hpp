/**
 * @file ApiKey.hpp
 * @brief API key / personal access token row (api_keys table, migration 005).
 *        The secret and its hash NEVER appear in the DTO — only metadata.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "utils/Time.hpp"

namespace Domain {

struct ApiKey {
    std::string id;
    std::string user_id;
    std::string name;
    std::string prefix;  // first chars of the key, e.g. "cpk_a1b2c3d4"
    // Timestamps are ISO 8601 — normalized in from_row().
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
        // API-facing timestamps are ISO 8601 UTC; libpqxx hands us Postgres
        // text form ("2026-07-26 05:34:32.876+00"). Convert at this single
        // DB→domain boundary, exactly as Post and AuditEntry do, so /api-keys
        // doesn't emit a different timestamp dialect than /posts.
        if (!row["last_used_at"].is_null())
            k.last_used_at = Utils::Time::pg_to_iso8601(row["last_used_at"].template as<std::string>());
        if (!row["revoked_at"].is_null())
            k.revoked_at = Utils::Time::pg_to_iso8601(row["revoked_at"].template as<std::string>());
        k.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
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
