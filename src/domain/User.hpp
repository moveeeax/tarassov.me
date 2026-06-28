/**
 * @file User.hpp
 * @brief User row. flask-base parity: app/models/user.py.
 *
 * Domain-only — no SQL here, no password hashing here, no JWT here.
 * Persistence lives in src/repositories/UserRepository.hpp; password
 * hashing in src/security/Password.hpp.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>

#include <nlohmann/json.hpp>

#include "domain/Role.hpp"

namespace Domain {

struct User {
    std::string id;     // UUID v4 (text)
    std::string email;  // CITEXT, unique
    // Nullable so the invite flow can create a user before they set a
    // password (parity with flask-base User.password_hash being NULL
    // until /join-from-invite/).
    std::optional<std::string> password_hash;
    std::optional<std::string> first_name;
    std::optional<std::string> last_name;
    bool confirmed{false};
    int role_id{0};
    std::string created_at;
    std::string updated_at;

    // The owning Role row, when joined. Empty optional means "not loaded
    // in this query"; not the same as "no role" (every user has one).
    std::optional<Role> role;

    template <typename Row>
    static User from_row(const Row& row) {
        User u;
        u.id = row["id"].template as<std::string>();
        u.email = row["email"].template as<std::string>();
        if (!row["password_hash"].is_null())
            u.password_hash = row["password_hash"].template as<std::string>();
        if (!row["first_name"].is_null())
            u.first_name = row["first_name"].template as<std::string>();
        if (!row["last_name"].is_null())
            u.last_name = row["last_name"].template as<std::string>();
        u.confirmed = row["confirmed"].template as<bool>();
        u.role_id = row["role_id"].template as<int>();
        u.created_at = row["created_at"].template as<std::string>();
        u.updated_at = row["updated_at"].template as<std::string>();
        return u;
    }

    std::string full_name() const {
        std::string out;
        if (first_name)
            out += *first_name;
        if (first_name && last_name)
            out += " ";
        if (last_name)
            out += *last_name;
        return out.empty() ? email : out;
    }

    /**
     * @brief Check a permission bit. flask-base parity: User.can().
     * @details Requires `role` to be loaded — controllers should always
     *          query through repository methods that join the role.
     */
    bool can(std::uint32_t perm) const { return role && role->has(perm); }
    bool is_admin() const { return role && role->is_admin(); }
};

/**
 * @brief Public JSON shape — what we send to clients. Never include
 *        password_hash; mark it explicit so a careless field add doesn't
 *        leak it.
 */
inline void to_json(nlohmann::json& j, const User& u) {
    j = nlohmann::json{
        {"id", u.id},
        {"email", u.email},
        {"first_name", u.first_name ? nlohmann::json(*u.first_name) : nlohmann::json(nullptr)},
        {"last_name", u.last_name ? nlohmann::json(*u.last_name) : nlohmann::json(nullptr)},
        {"full_name", u.full_name()},
        {"confirmed", u.confirmed},
        {"role_id", u.role_id},
        {"created_at", u.created_at},
        {"updated_at", u.updated_at},
    };
    if (u.role) {
        j["role"] = *u.role;
    }
}

}  // namespace Domain
