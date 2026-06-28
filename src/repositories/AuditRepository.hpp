/**
 * @file AuditRepository.hpp
 * @brief Read access to the audit_log table (migration 003). find/list/count
 *        come from CrudBase; list_filtered adds the optional action/actor/
 *        target/date filters the admin audit view needs, all bound as
 *        parameters (no string-built SQL).
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/AuditEntry.hpp"
#include "repositories/CrudBase.hpp"

namespace Repositories {

class AuditRepository : public CrudBase<AuditRepository, Domain::AuditEntry, long long> {
public:
    // CrudBase contract.
    static constexpr const char* kTable = "audit_log";
    static constexpr const char* kColumns = "id, actor_id, action, target_type, target_id, details, created_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";

    struct Filters {
        std::optional<std::string> action;
        std::optional<std::string> actor_id;
        std::optional<std::string> target_type;
        std::optional<std::string> from;  // ISO-8601 lower bound (created_at >=)
        std::optional<std::string> to;    // ISO-8601 upper bound (created_at <=)
    };

    struct Page {
        std::vector<Domain::AuditEntry> entries;
        long total{0};
    };

    /// Filtered + paginated newest-first. A nullopt filter is "no constraint"
    /// ($N::type IS NULL short-circuits the clause), so one fixed query covers
    /// every combination without concatenating user input into SQL.
    Page list_filtered(const Filters& f, int limit, int offset) {
        return Database::get().execute_read([&](auto& txn) {
            const std::string where =
                " WHERE ($1::text IS NULL OR action = $1)"
                "   AND ($2::text IS NULL OR actor_id = $2)"
                "   AND ($3::text IS NULL OR target_type = $3)"
                "   AND ($4::timestamptz IS NULL OR created_at >= $4::timestamptz)"
                "   AND ($5::timestamptz IS NULL OR created_at <= $5::timestamptz)";

            auto rows = txn.exec_params(
                "SELECT id, actor_id, action, target_type, target_id, details, created_at "
                "FROM audit_log" +
                    where + " ORDER BY created_at DESC, id DESC LIMIT $6 OFFSET $7",
                f.action,
                f.actor_id,
                f.target_type,
                f.from,
                f.to,
                limit,
                offset);
            Page p;
            p.entries.reserve(rows.size());
            for (const auto& r : rows)
                p.entries.push_back(Domain::AuditEntry::from_row(r));

            auto cnt = txn.exec_params(
                "SELECT COUNT(*) FROM audit_log" + where, f.action, f.actor_id, f.target_type, f.from, f.to);
            p.total = cnt.at(0).at(0).template as<long>();
            return p;
        });
    }
};

}  // namespace Repositories
