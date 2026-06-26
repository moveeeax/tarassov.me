/**
 * @file AuditEntry.hpp
 * @brief One row of the audit trail (audit_log, migration 003). Written by
 *        Security::Audit::record(); read back via AuditRepository for the
 *        admin audit view (GET /api/admin/audit).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

#include <nlohmann/json.hpp>

namespace Domain {

struct AuditEntry {
    long long id{0};
    std::optional<std::string> actor_id;  // principal subject (uuid) or null = system
    std::string action;                   // e.g. "user.create"
    std::string target_type;              // e.g. "user"
    std::optional<std::string> target_id;
    nlohmann::json details = nlohmann::json::object();
    std::string created_at;

    template <typename Row>
    static AuditEntry from_row(const Row& row) {
        AuditEntry a;
        a.id = row["id"].template as<long long>();
        if (!row["actor_id"].is_null())
            a.actor_id = row["actor_id"].template as<std::string>();
        a.action = row["action"].template as<std::string>();
        a.target_type = row["target_type"].template as<std::string>();
        if (!row["target_id"].is_null())
            a.target_id = row["target_id"].template as<std::string>();
        if (!row["details"].is_null()) {
            try {
                a.details = nlohmann::json::parse(row["details"].template as<std::string>());
            } catch (...) {
                a.details = nlohmann::json::object();
            }
        }
        a.created_at = row["created_at"].template as<std::string>();
        return a;
    }
};

inline void to_json(nlohmann::json& j, const AuditEntry& a) {
    j = nlohmann::json{
        {"id", a.id},
        {"actor_id", a.actor_id ? nlohmann::json(*a.actor_id) : nlohmann::json(nullptr)},
        {"action", a.action},
        {"target_type", a.target_type},
        {"target_id", a.target_id ? nlohmann::json(*a.target_id) : nlohmann::json(nullptr)},
        {"details", a.details},
        {"created_at", a.created_at},
    };
}

}  // namespace Domain
