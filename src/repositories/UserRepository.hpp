/**
 * @file UserRepository.hpp
 * @brief All SQL touching `users` lives here.
 *
 * flask-base parity: app/models/user.py + the queries scattered across
 * app/account/views.py and app/admin/views.py. Everything is funnelled
 * through one class so controllers never touch pqxx directly.
 *
 * Constraint violations surface as typed exceptions (DuplicateEmail,
 * UserNotFound) so the HTTP layer can map them to 409/404 without
 * sniffing pqxx::sql_error::sqlstate() every time.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Repositories {

// The 409 code / 404 resource the HTTP layer reports live ON the exception now,
// so the shared with_repo_errors() helper maps them without knowing this type.
struct DuplicateEmail : ConflictError {
    DuplicateEmail() : ConflictError("email_taken", "Email is already registered") {}
};

struct UserNotFound : NotFoundError {
    UserNotFound() : NotFoundError("user") {}
};

class UserRepository {
public:
    /**
     * @brief Columns selected by every query that returns a User joined
     *        with its Role. Kept once so all loaders agree on the shape.
     */
    static constexpr const char* kSelectWithRole =
        "SELECT u.id, u.email, u.password_hash, u.first_name, u.last_name, "
        "       u.confirmed, u.role_id, u.created_at, u.updated_at, "
        "       r.id  AS r_id, r.name AS r_name, r.permissions AS r_permissions, "
        "       r.is_default AS r_is_default, r.created_at AS r_created_at "
        "FROM users u JOIN roles r ON r.id = u.role_id ";

    template <typename Row>
    static Domain::User user_with_role_from_row(const Row& row) {
        Domain::User u = Domain::User::from_row(row);
        Domain::Role r;
        r.id = row["r_id"].template as<int>();
        r.name = row["r_name"].template as<std::string>();
        r.permissions = static_cast<std::uint32_t>(row["r_permissions"].template as<int>());
        r.is_default = row["r_is_default"].template as<bool>();
        r.created_at = row["r_created_at"].template as<std::string>();
        u.role = std::move(r);
        return u;
    }

    /**
     * @param from_primary Read from the primary instead of a replica. Set when
     *        re-reading a row the same request/job just wrote (read-after-write)
     *        so replica lag can't return a stale row or a spurious "not found".
     */
    std::optional<Domain::User> find(const std::string& id, bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Domain::User> {
            auto r = txn.exec_params(std::string(kSelectWithRole) + "WHERE u.id = $1", id);
            if (r.empty())
                return std::nullopt;
            return user_with_role_from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    std::optional<Domain::User> find_by_email(const std::string& email) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Domain::User> {
            auto r = txn.exec_params(std::string(kSelectWithRole) + "WHERE u.email = $1", email);
            if (r.empty())
                return std::nullopt;
            return user_with_role_from_row(r[0]);
        });
    }

    std::vector<Domain::User> list(int limit = 100, int offset = 0) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                std::string(kSelectWithRole) + "ORDER BY u.created_at DESC LIMIT $1 OFFSET $2", limit, offset);
            std::vector<Domain::User> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(user_with_role_from_row(row));
            return out;
        });
    }

    /**
     * @brief INSERT a new user. Throws DuplicateEmail on UNIQUE violation
     *        (SQLSTATE 23505) so the caller can return 409.
     *
     * @param password_hash Optional. Pass nullopt for invite-only flow
     *                      where the user sets their password later.
     */
    Domain::User create(const std::string& email,
                        std::optional<std::string> password_hash,
                        std::optional<std::string> first_name,
                        std::optional<std::string> last_name,
                        int role_id,
                        bool confirmed = false) {
        return detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO users (email, password_hash, first_name, last_name, role_id, confirmed) "
                        "VALUES ($1, $2, $3, $4, $5, $6) "
                        "RETURNING id, email, password_hash, first_name, last_name, confirmed, "
                        "          role_id, created_at, updated_at",
                        email,
                        password_hash,
                        first_name,
                        last_name,
                        role_id,
                        confirmed);
                    // The role isn't joined here — callers that need it
                    // attach the Role they already loaded (no extra query).
                    return Domain::User::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateEmail{};
            });
    }

    void update_password_hash(const std::string& id, const std::string& new_hash) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE users SET password_hash = $1 WHERE id = $2 RETURNING id", new_hash, id);
            if (r.empty())
                throw UserNotFound{};
            return 0;
        });
    }

    /**
     * @brief Mark the user confirmed. Idempotent — re-confirming a
     *        confirmed user is a no-op, returns true.
     * @note Deliberately returns bool (false = no such user) instead of
     *       throwing UserNotFound like the other mutators: confirmation is a
     *       fire-once flow where "no row" is an expected branch the caller
     *       maps to 404, not an exceptional repo error.
     */
    bool mark_confirmed(const std::string& id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE users SET confirmed = TRUE WHERE id = $1 RETURNING id", id);
            return !r.empty();
        });
    }

    /**
     * @brief Redeem an invite: set the initial password and confirm the account
     *        in a single write. The WHERE clause makes this a true one-shot at
     *        the DB layer — it only matches a still-pending invite (no password
     *        yet, unconfirmed), so a replayed/captured invite link cannot reset
     *        an already-active account's password (the Redis replay guard is
     *        fail-open and must not be the only defense).
     * @return true if the invite was redeemed; false if no pending invite
     *         matched (already redeemed, already confirmed, or gone).
     */
    bool redeem_invite(const std::string& id, const std::string& password_hash) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE users SET password_hash = $1, confirmed = TRUE "
                "WHERE id = $2 AND confirmed = FALSE AND password_hash IS NULL RETURNING id",
                password_hash,
                id);
            return !r.empty();
        });
    }

    /**
     * @brief Change email. Throws DuplicateEmail on unique-violation,
     *        UserNotFound if the user is gone.
     */
    void change_email(const std::string& id, const std::string& new_email) {
        detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params("UPDATE users SET email = $1 WHERE id = $2 RETURNING id", new_email, id);
                    if (r.empty())
                        throw UserNotFound{};
                    return 0;
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateEmail{};
            });
    }

    /**
     * @brief Update first/last name. Pass nullopt to keep a field's current
     *        value (COALESCE). Throws UserNotFound if the row is gone.
     */
    void update_names(const std::string& id,
                      std::optional<std::string> first_name,
                      std::optional<std::string> last_name) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE users SET "
                "  first_name = COALESCE($1, first_name), "
                "  last_name  = COALESCE($2, last_name) "
                "WHERE id = $3 RETURNING id",
                first_name,
                last_name,
                id);
            if (r.empty())
                throw UserNotFound{};
            return 0;
        });
    }

    /**
     * @brief Admin partial update — email / role / names in ONE statement
     *        (and therefore one transaction), so a constraint failure can't
     *        leave a half-applied patch the way three sequential mutations
     *        could. Pass nullopt to keep a field's current value.
     *        Throws DuplicateEmail (23505) and UserNotFound like the
     *        single-field mutators.
     */
    void admin_update(const std::string& id,
                      std::optional<std::string> email,
                      std::optional<int> role_id,
                      std::optional<std::string> first_name,
                      std::optional<std::string> last_name) {
        detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "UPDATE users SET "
                        "  email      = COALESCE($1, email), "
                        "  role_id    = COALESCE($2, role_id), "
                        "  first_name = COALESCE($3, first_name), "
                        "  last_name  = COALESCE($4, last_name) "
                        "WHERE id = $5 RETURNING id",
                        email,
                        role_id,
                        first_name,
                        last_name,
                        id);
                    if (r.empty())
                        throw UserNotFound{};
                    return 0;
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateEmail{};
            });
    }

    void change_role(const std::string& id, int new_role_id) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE users SET role_id = $1 WHERE id = $2 RETURNING id", new_role_id, id);
            if (r.empty())
                throw UserNotFound{};
            return 0;
        });
    }

    void remove(const std::string& id) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("DELETE FROM users WHERE id = $1 RETURNING id", id);
            if (r.empty())
                throw UserNotFound{};
            return 0;
        });
    }

    /**
     * @brief Total count for pagination headers.
     */
    long count() {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec("SELECT COUNT(*) FROM users");
            return r[0][0].template as<long>();
        });
    }
};

}  // namespace Repositories
