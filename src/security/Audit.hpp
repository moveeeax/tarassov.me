/**
 * @file Audit.hpp
 * @brief Append-only audit trail for privileged actions (audit_log table,
 *        migration 003). One free function: Security::Audit::record(...).
 *
 *        Best-effort and NEVER throws — an audit-write failure must not break or
 *        roll back the action it records (same fail-open posture as metrics).
 *        Call it AFTER the action's own write has succeeded.
 */

#pragma once

#include <optional>
#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"

namespace Security::Audit {

using json = nlohmann::json;

/**
 * @param actor_id  Acting principal's subject (uuid); empty → recorded as NULL.
 * @param action    Dotted verb, e.g. "user.create", "role.delete".
 * @param target_type  Entity kind, e.g. "user", "role".
 * @param target_id    Affected entity id (uuid or int rendered as text).
 * @param details   Optional action-specific JSON context (no secrets).
 */
inline void record(const std::string& actor_id,
                   const std::string& action,
                   const std::string& target_type,
                   const std::string& target_id,
                   const json& details = json::object()) {
    try {
        Database::get().execute_write([&](auto& txn) {
            std::optional<std::string> actor = actor_id.empty() ? std::nullopt : std::optional<std::string>{actor_id};
            txn.exec_params(
                "INSERT INTO audit_log (actor_id, action, target_type, target_id, details) "
                "VALUES ($1, $2, $3, $4, $5::jsonb)",
                actor,
                action,
                target_type,
                target_id,
                details.dump());
            return 0;
        });
    } catch (const std::exception& e) {
        // Don't let the trail break the action — log and move on.
        spdlog::warn("audit: failed to record {} on {}:{} ({})", action, target_type, target_id, e.what());
    }
}

}  // namespace Security::Audit
