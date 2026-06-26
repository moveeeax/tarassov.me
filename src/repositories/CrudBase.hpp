/**
 * @file CrudBase.hpp
 * @brief Header-only CRTP base that supplies the mechanical read methods
 *        (find / list / count) every table repository otherwise re-implements,
 *        so a new resource only declares its table + columns and overrides the
 *        bespoke queries. The single biggest fork-velocity lever, and it stays
 *        header-only.
 *
 * A derived repo provides four static constants and an Entity with from_row():
 *   class FooRepository : public CrudBase<FooRepository, Domain::Foo, std::string> {
 *     public:
 *       static constexpr const char* kTable    = "foos";
 *       static constexpr const char* kColumns  = "id, name, created_at";
 *       static constexpr const char* kIdColumn = "id";
 *       static constexpr const char* kOrderBy  = "created_at DESC";
 *       // ... bespoke create/update/remove ...
 *   };
 *
 * KeyT is the id type (std::string for uuid PKs, int for serial PKs). Repos with
 * joined loads (e.g. UserRepository, which joins roles) keep their own queries;
 * the base is for the plain single-table case.
 *
 * ── Data-layer ergonomics conventions (recommended for new resources) ────────
 *  - Sorting/filtering: never concatenate a client `?sort=` column into SQL.
 *    Use Repositories::safe_order_by() (ListQuery.hpp) with a column allowlist.
 *  - Column single-source: keep the SELECT column list (kColumns), the struct
 *    fields, from_row(), and to_json() in sync — adding a column means touching
 *    all four. Prefer driving them from one place (an X-macro or a single
 *    constexpr column array) so they can't silently drift (a forgotten from_row
 *    line reads a default; a forgotten to_json line drops the field).
 *  - Request DTOs: parse an inbound body once into a typed Create/Update struct
 *    (std::optional members + a from_json) instead of threading long positional
 *    optional args into repo methods.
 *  - Validation co-location: put one validate_<entity>() next to the DTO so the
 *    rules are testable without a controller.
 *  - Soft-delete: if you need it, add a `deleted_at TIMESTAMPTZ` column and a
 *    soft_delete() that sets it, and append `AND deleted_at IS NULL` to find/
 *    list. The template ships hard deletes by default — there is no global
 *    soft-delete, so adopt it per-resource deliberately.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"

namespace Repositories {

template <typename Derived, typename Entity, typename KeyT = std::string>
class CrudBase {
public:
    /// Fetch by primary key. @p from_primary forces the primary (read-after-write).
    std::optional<Entity> find(const KeyT& id, bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Entity> {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kIdColumn + " = $1", id);
            if (r.empty())
                return std::nullopt;
            return Entity::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    std::vector<Entity> list(int limit = 100, int offset = 0) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                select_prefix() + " ORDER BY " + Derived::kOrderBy + " LIMIT $1 OFFSET $2", limit, offset);
            std::vector<Entity> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Entity::from_row(row));
            return out;
        });
    }

    long count() {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec("SELECT COUNT(*) FROM " + std::string(Derived::kTable));
            return r.at(0).at(0).template as<long>();
        });
    }

    // ── Ownership-scoped reads ────────────────────────────────────────────
    // For per-user resources: every query carries `WHERE <kOwnerColumn> = $owner`
    // so one user can NEVER read another's rows (the default global find/list
    // would be an IDOR for a user-facing resource). Only repos that declare a
    // `static constexpr const char* kOwnerColumn` instantiate these — global
    // resources are unaffected. Pair with API_REQUIRE_OWNER in the controller.
    std::optional<Entity> find_owned(const KeyT& id, const std::string& owner_id, bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Entity> {
            auto r = txn.exec_params(
                select_prefix() + " WHERE " + Derived::kIdColumn + " = $1 AND " + Derived::kOwnerColumn + " = $2",
                id,
                owner_id);
            if (r.empty())
                return std::nullopt;
            return Entity::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    std::vector<Entity> list_owned(const std::string& owner_id, int limit = 100, int offset = 0) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kOwnerColumn + " = $1 ORDER BY " +
                                         Derived::kOrderBy + " LIMIT $2 OFFSET $3",
                                     owner_id,
                                     limit,
                                     offset);
            std::vector<Entity> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Entity::from_row(row));
            return out;
        });
    }

    long count_owned(const std::string& owner_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM " + std::string(Derived::kTable) + " WHERE " + Derived::kOwnerColumn + " = $1",
                owner_id);
            return r.at(0).at(0).template as<long>();
        });
    }

private:
    static std::string select_prefix() {
        return "SELECT " + std::string(Derived::kColumns) + " FROM " + std::string(Derived::kTable);
    }
};

}  // namespace Repositories
