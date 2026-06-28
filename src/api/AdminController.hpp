/**
 * @file AdminController.hpp
 * @brief Admin user-management endpoints.
 *
 * flask-base parity: app/admin/views.py — same routes, JSON shape
 * instead of HTML+flash. Every handler is gated by require_admin().
 *
 * Routes (all under /api/admin):
 *   GET    /api/admin/users                      list users (paginated)
 *   POST   /api/admin/users                      create a fully-formed user
 *   POST   /api/admin/invite                     invite via email — user sets password later
 *   GET    /api/admin/users/{id}                 user detail
 *   PATCH  /api/admin/users/{id}                 partial update (email / role / first/last name)
 *   DELETE /api/admin/users/{id}                 delete user
 *   GET    /api/admin/roles                      list roles
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "email/AccountEmails.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Audit.hpp"
#include "security/Auth.hpp"
#include "security/Password.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AdminController : public HttpController<AdminController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminController::listUsers, "/api/v1/admin/users", Get);
    ADD_METHOD_TO(AdminController::createUser, "/api/v1/admin/users", Post);
    ADD_METHOD_TO(AdminController::inviteUser, "/api/v1/admin/invite", Post);
    ADD_METHOD_TO(AdminController::getUser, "/api/v1/admin/users/{1}", Get);
    ADD_METHOD_TO(AdminController::updateUser, "/api/v1/admin/users/{1}", Patch);
    ADD_METHOD_TO(AdminController::deleteUser, "/api/v1/admin/users/{1}", Delete);
    ADD_METHOD_TO(AdminController::listRoles, "/api/v1/admin/roles", Get);
    ADD_METHOD_TO(AdminController::createRole, "/api/v1/admin/roles", Post);
    ADD_METHOD_TO(AdminController::updateRole, "/api/v1/admin/roles/{1}", Patch);
    ADD_METHOD_TO(AdminController::deleteRole, "/api/v1/admin/roles/{1}", Delete);
    METHOD_LIST_END

    void listUsers(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "admin listUsers", [&] {
            Repositories::UserRepository repo;
            auto users = repo.list(page.limit, page.offset);
            long total = repo.count();
            json data = json::array();
            for (const auto& u : users)
                data.push_back(u);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    void createUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "email");
        Validation::require(errs, body, "password");
        Validation::email(errs, body, "email");
        Validation::string_length(errs, body, "password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        // role_id optional — defaults to "User" role.
        std::optional<int> requested_role_id;
        if (body.contains("role_id") && body["role_id"].is_number_integer())
            requested_role_id = body["role_id"].get<int>();

        Repositories::RoleRepository roles;
        auto role = requested_role_id ? roles.find(*requested_role_id) : roles.find_default();
        if (!role) {
            callback(ErrorResponse::bad_request("invalid_role", "Role does not exist"));
            return;
        }

        with_repo_errors(callback, "admin createUser", [&] {
            const std::string hash = Security::Password::hash(body["password"].get<std::string>());
            // Admin-created users land already-confirmed by default —
            // matches flask-base where /admin/new-user skips the email
            // confirmation step.
            Repositories::UserRepository users;
            auto created = users.create(body["email"].get<std::string>(),
                                        hash,
                                        Validation::opt_string(body, "first_name"),
                                        Validation::opt_string(body, "last_name"),
                                        role->id,
                                        /*confirmed=*/true);
            // Attach the role we already loaded instead of re-querying.
            created.role = *role;
            Security::Audit::record(actor_of(req), "user.create", "user", created.id, {{"email", created.email}});
            callback(Response::created({{"data", json(created)}}));
        });
    }

    void inviteUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "email");
        Validation::email(errs, body, "email");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        std::optional<int> requested_role_id;
        if (body.contains("role_id") && body["role_id"].is_number_integer())
            requested_role_id = body["role_id"].get<int>();
        Repositories::RoleRepository roles;
        auto role = requested_role_id ? roles.find(*requested_role_id) : roles.find_default();
        if (!role) {
            callback(ErrorResponse::bad_request("invalid_role"));
            return;
        }

        with_repo_errors(callback, "admin inviteUser", [&] {
            Repositories::UserRepository users;
            // No password yet — they'll set one via the invite link.
            auto created = users.create(body["email"].get<std::string>(),
                                        std::nullopt,
                                        Validation::opt_string(body, "first_name"),
                                        Validation::opt_string(body, "last_name"),
                                        role->id,
                                        /*confirmed=*/false);
            // Attach the role we already loaded instead of re-querying.
            created.role = *role;
            Email::AccountEmails::send_invite(created);
            Security::Audit::record(actor_of(req), "user.invite", "user", created.id, {{"email", created.email}});
            callback(Response::created({{"data", json(created)}, {"message", "Invitation sent"}}));
        });
    }

    void getUser(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed user id"));
            return;
        }
        with_repo_errors(callback, "admin getUser", [&] {
            Repositories::UserRepository repo;
            auto user = repo.find(id);
            if (!user) {
                callback(ErrorResponse::not_found("user"));
                return;
            }
            callback(Response::ok({{"data", *user}}));
        });
    }

    void updateUser(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed user id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Self-protection: an admin can't change their own role away from
        // admin (flask-base does the same check). Otherwise the very last
        // admin can lock everyone out by accident.
        auto principal = Security::Auth::principal_of(req);
        const bool changing_self = principal && principal->subject == id;

        Repositories::UserRepository users;

        with_repo_errors(callback, "admin updateUser", [&] {
            std::optional<std::string> new_email;
            if (body.contains("email") && body["email"].is_string()) {
                Validation::Errors e;
                Validation::email(e, body, "email");
                if (e.any()) {
                    callback(Validation::response_400(e));
                    return;
                }
                new_email = body["email"].get<std::string>();
            }
            std::optional<int> new_role_id;
            if (body.contains("role_id") && body["role_id"].is_number_integer()) {
                if (changing_self) {
                    callback(ErrorResponse::bad_request(
                        "self_role_change", "You cannot change the role of your own account; ask another admin"));
                    return;
                }
                Repositories::RoleRepository roles;
                if (!roles.find(body["role_id"].get<int>())) {
                    callback(ErrorResponse::bad_request("invalid_role"));
                    return;
                }
                new_role_id = body["role_id"].get<int>();
            }
            const auto first_name = Validation::opt_string(body, "first_name");
            const auto last_name = Validation::opt_string(body, "last_name");

            // One repository call → one transaction: a constraint failure
            // (e.g. duplicate email) can't leave the role half-changed the
            // way three sequential mutations could.
            if (new_email || new_role_id || first_name || last_name) {
                users.admin_update(id, new_email, new_role_id, first_name, last_name);
            }
            // Read from the primary so the echoed row reflects the write we
            // just made — a lagging replica would return the pre-update values.
            auto fresh = users.find(id, /*from_primary=*/true);
            if (!fresh) {
                callback(ErrorResponse::not_found("user"));
                return;
            }
            Security::Audit::record(actor_of(req), "user.update", "user", id);
            callback(Response::ok({{"data", *fresh}}));
        });
    }

    void deleteUser(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed user id"));
            return;
        }
        // Self-protection — flask-base parity: app/admin/views.py
        // delete_user explicitly refuses to delete current_user.
        auto principal = Security::Auth::principal_of(req);
        if (principal && principal->subject == id) {
            callback(
                ErrorResponse::bad_request("self_delete", "You cannot delete your own account; ask another admin"));
            return;
        }
        with_repo_errors(callback, "admin deleteUser", [&] {
            Repositories::UserRepository repo;
            repo.remove(id);
            Security::Audit::record(actor_of(req), "user.delete", "user", id);
            callback(Response::ok({{"message", "User deleted"}}));
        });
    }

    void listRoles(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        with_repo_errors(callback, "admin listRoles", [&] {
            Repositories::RoleRepository repo;
            // CrudBase::list defaults to LIMIT 100; roles are few but pass a
            // high cap so the list isn't silently truncated (was unbounded
            // before the CrudBase refactor).
            auto roles = repo.list(1000);
            json data = json::array();
            for (const auto& r : roles)
                data.push_back(r);
            callback(Response::list(data));
        });
    }

    void createRole(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "name");
        Validation::string_length(errs, body, "name", 1, 64);
        if (!body.contains("permissions") || !body["permissions"].is_number_integer()) {
            errs.add("permissions", "invalid_type", "must be an integer bitmask");
        }
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        const auto perms = static_cast<std::uint32_t>(body["permissions"].get<long>());
        const bool is_default = body.value("is_default", false);
        with_repo_errors(callback, "admin createRole", [&] {
            Repositories::RoleRepository repo;
            auto created = repo.create(body["name"].get<std::string>(), perms, is_default);
            Security::Audit::record(
                actor_of(req), "role.create", "role", std::to_string(created.id), {{"name", created.name}});
            callback(Response::created({{"data", json(created)}}));
        });
    }

    void updateRole(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id_str) {
        API_REQUIRE_ADMIN(req, callback);
        const int id = parse_int(id_str, -1);
        if (id <= 0) {
            callback(ErrorResponse::bad_request("invalid_id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        std::optional<std::string> name;
        std::optional<std::uint32_t> permissions;
        std::optional<bool> is_default;
        if (body.contains("name") && body["name"].is_string())
            name = body["name"].get<std::string>();
        if (body.contains("permissions") && body["permissions"].is_number_integer())
            permissions = static_cast<std::uint32_t>(body["permissions"].get<long>());
        if (body.contains("is_default") && body["is_default"].is_boolean())
            is_default = body["is_default"].get<bool>();
        if (!name && !permissions && !is_default) {
            callback(
                ErrorResponse::bad_request("empty_patch", "Provide at least one of name / permissions / is_default"));
            return;
        }
        with_repo_errors(callback, "admin updateRole", [&] {
            Repositories::RoleRepository repo;
            auto updated = repo.update(id, name, permissions, is_default);
            Security::Audit::record(actor_of(req), "role.update", "role", std::to_string(id));
            callback(Response::ok({{"data", json(updated)}}));
        });
    }

    void deleteRole(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id_str) {
        API_REQUIRE_ADMIN(req, callback);
        const int id = parse_int(id_str, -1);
        if (id <= 0) {
            callback(ErrorResponse::bad_request("invalid_id"));
            return;
        }
        with_repo_errors(callback, "admin deleteRole", [&] {
            Repositories::RoleRepository repo;
            // Self-protection: refuse to delete the default role —
            // future sign-ups would have nowhere to land.
            auto existing = repo.find(id);
            if (existing && existing->is_default) {
                callback(ErrorResponse::bad_request(
                    "default_role_protected",
                    "The default role cannot be deleted; promote another role to default first"));
                return;
            }
            repo.remove(id);
            Security::Audit::record(actor_of(req), "role.delete", "role", std::to_string(id));
            callback(Response::ok({{"message", "Role deleted"}}));
        });
    }

private:
    /// Acting admin's principal subject for the audit trail ("" when auth off).
    static std::string actor_of(const HttpRequestPtr& req) {
        auto p = Security::Auth::principal_of(req);
        return p ? p->subject : std::string{};
    }
};

}  // namespace Api
