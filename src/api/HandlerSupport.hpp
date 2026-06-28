/**
 * @file HandlerSupport.hpp
 * @brief Shared controller-side helpers: map repository exceptions to the
 *        canonical HTTP error responses in one place.
 * @details Every mutating handler used to repeat the same try/catch ladder
 *          (DuplicateEmail->409, *NotFound->404, RoleInUse->409, std::exception
 *          ->500). That's ~13 copies that drift in their codes/messages and
 *          that a new handler can forget. with_repo_errors() centralizes the
 *          mapping so the error contract is defined once.
 *
 *          The repository layer still owns the BOUNDARY (it throws typed
 *          exceptions, it does not know about HTTP); this helper is the api-
 *          side translation of those types — the natural counterpart to
 *          Repositories::detail::translate_sql (SQLSTATE -> exception).
 *
 *          It catches the GENERIC bases (Repositories::NotFoundError /
 *          ConflictError, defined in RepoErrors.hpp), NOT the concrete domain
 *          exceptions — so this header does not depend on UserRepository /
 *          RoleRepository, and a forked domain's own NotFoundError/ConflictError
 *          subclasses map automatically with no edit here.
 */

#pragma once

#include <exception>
#include <functional>

#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include "repositories/RepoErrors.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

/**
 * @brief Run @p fn, translating any repository exception into the canonical
 *        HTTP error response via @p cb. @p op is a short label for the 500
 *        log line ("admin createUser"). Returns true if @p fn completed
 *        without throwing — handlers that must keep running after the guarded
 *        block (e.g. to send a success response) can branch on it.
 *
 * The concrete domain exceptions carry their own code/resource and derive
 * from these bases, so the same stable machine codes the tests assert hold:
 *   DuplicateEmail -> 409 email_taken | UserNotFound -> 404 user
 *   DuplicateRole  -> 409 role_exists | RoleNotFound -> 404 role
 *   RoleInUse      -> 409 role_in_use | anything else -> 500
 */
template <typename Fn>
inline bool with_repo_errors(const std::function<void(const drogon::HttpResponsePtr&)>& cb, const char* op, Fn&& fn) {
    try {
        fn();
        return true;
    } catch (const Repositories::ConflictError& e) {
        cb(ErrorResponse::conflict(e.code(), e.message()));
    } catch (const Repositories::NotFoundError& e) {
        cb(ErrorResponse::not_found(e.resource()));
    } catch (const std::exception& e) {
        spdlog::error("{} failed: {}", op, e.what());
        cb(ErrorResponse::internal_error());
    }
    return false;
}

}  // namespace Api
