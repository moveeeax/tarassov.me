/**
 * @file ApiKeyRepository.hpp
 * @brief Owner-scoped access to the api_keys table (migration 005). The hot
 *        auth-by-hash lookup lives in security/ApiKeys.hpp; this repository is
 *        the management surface (create / list / revoke), always scoped to the
 *        owning user so one user can never see or revoke another's keys.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/ApiKey.hpp"

namespace Repositories {

class ApiKeyRepository {
public:
    // Metadata columns only — key_hash is write-only from here.
    static constexpr const char* kColumns = "id, user_id, name, prefix, last_used_at, revoked_at, created_at";

    Domain::ApiKey create(const std::string& user_id,
                          const std::string& name,
                          const std::string& key_hash,
                          const std::string& prefix) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                std::string("INSERT INTO api_keys (user_id, name, key_hash, prefix) VALUES ($1, $2, $3, $4) "
                            "RETURNING ") +
                    kColumns,
                user_id,
                name,
                key_hash,
                prefix);
            return Domain::ApiKey::from_row(r[0]);
        });
    }

    std::vector<Domain::ApiKey> list_for_user(const std::string& user_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                std::string("SELECT ") + kColumns + " FROM api_keys WHERE user_id = $1 ORDER BY created_at DESC",
                user_id);
            std::vector<Domain::ApiKey> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Domain::ApiKey::from_row(row));
            return out;
        });
    }

    /// Soft-revoke a key the caller owns. Returns false if it doesn't exist, is
    /// already revoked, or belongs to someone else (no IDOR, no info leak).
    bool revoke(const std::string& id, const std::string& user_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE api_keys SET revoked_at = now() "
                "WHERE id = $1 AND user_id = $2 AND revoked_at IS NULL RETURNING id",
                id,
                user_id);
            return !r.empty();
        });
    }
};

}  // namespace Repositories
