/**
 * @file RoleRepository.hpp
 * @brief All SQL touching `roles` lives here.
 *
 * flask-base parity: app/models/user.py — Role queries via SQLAlchemy.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Repositories {

// Stable 409 codes / 404 resource carried on the exception, so with_repo_errors()
// maps them without including this header.
struct DuplicateRole : ConflictError {
    DuplicateRole() : ConflictError("role_exists", "A role with that name already exists") {}
};

struct RoleNotFound : NotFoundError {
    RoleNotFound() : NotFoundError("role") {}
};

struct RoleInUse : ConflictError {
    RoleInUse() : ConflictError("role_in_use", "Reassign users away from this role before deleting") {}
};

class RoleRepository : public CrudBase<RoleRepository, Domain::Role, int> {
public:
    // CrudBase contract — supplies find(id) / list(limit,offset) / count().
    static constexpr const char* kTable = "roles";
    static constexpr const char* kColumns = "id, name, permissions, is_default, created_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "id";

    std::optional<Domain::Role> find_by_name(const std::string& name) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Domain::Role> {
            auto r = txn.exec_params("SELECT id, name, permissions, is_default, created_at FROM roles WHERE name = $1",
                                     name);
            if (r.empty())
                return std::nullopt;
            return Domain::Role::from_row(r[0]);
        });
    }

    /**
     * @brief Default role for new sign-ups. Migration 001 marks "User" as
     *        the default; this query is idempotent if the seed didn't run.
     */
    std::optional<Domain::Role> find_default() {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Domain::Role> {
            auto r = txn.exec(
                "SELECT id, name, permissions, is_default, created_at "
                "FROM roles WHERE is_default = TRUE LIMIT 1");
            if (r.empty())
                return std::nullopt;
            return Domain::Role::from_row(r[0]);
        });
    }

    /**
     * @brief Insert a new role. Throws DuplicateRole on UNIQUE(name).
     *        is_default=true triggers a transaction-wide flip: only one
     *        row may be the default at a time, so we clear any prior
     *        default first. Migration 001 enforces the same invariant
     *        with a partial UNIQUE index.
     */
    Domain::Role create(const std::string& name, std::uint32_t permissions, bool is_default) {
        return detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    if (is_default)
                        txn.exec("UPDATE roles SET is_default = FALSE WHERE is_default = TRUE");
                    auto r = txn.exec_params(
                        "INSERT INTO roles (name, permissions, is_default) VALUES ($1, $2, $3) "
                        "RETURNING id, name, permissions, is_default, created_at",
                        name,
                        static_cast<int>(permissions),
                        is_default);
                    return Domain::Role::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateRole{};
            });
    }

    /**
     * @brief Partial update: pass nullopt for any field that should keep
     *        its current value. Throws RoleNotFound on missing id and
     *        DuplicateRole on UNIQUE(name) collision.
     */
    Domain::Role update(int id,
                        std::optional<std::string> name,
                        std::optional<std::uint32_t> permissions,
                        std::optional<bool> is_default) {
        return detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    if (is_default && *is_default) {
                        // Clear any existing default before flipping this row on.
                        txn.exec_params("UPDATE roles SET is_default = FALSE WHERE id <> $1 AND is_default = TRUE", id);
                    }
                    auto r = txn.exec_params(
                        "UPDATE roles SET "
                        "  name        = COALESCE($1, name), "
                        "  permissions = COALESCE($2, permissions), "
                        "  is_default  = COALESCE($3, is_default) "
                        "WHERE id = $4 "
                        "RETURNING id, name, permissions, is_default, created_at",
                        name,
                        permissions ? std::optional<int>(static_cast<int>(*permissions)) : std::nullopt,
                        is_default,
                        id);
                    if (r.empty())
                        throw RoleNotFound{};
                    return Domain::Role::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateRole{};
            });
    }

    /**
     * @brief Delete a role. Throws RoleNotFound if it doesn't exist,
     *        RoleInUse on FK violation (SQLSTATE 23503) — the migration's
     *        ON DELETE RESTRICT keeps the schema consistent.
     */
    void remove(int id) {
        detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params("DELETE FROM roles WHERE id = $1 RETURNING id", id);
                    if (r.empty())
                        throw RoleNotFound{};
                    return 0;
                });
            },
            [](std::string_view ss) {
                if (ss == "23503")
                    throw RoleInUse{};
            });
    }
};

}  // namespace Repositories
