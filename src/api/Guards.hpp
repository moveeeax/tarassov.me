/**
 * @file Guards.hpp
 * @brief Uniform handler-entry guards shared by all controllers.
 * @details Expressed as macros so each guard is a single line that can't be
 *          half-written (the `callback(err); return;` pair stays atomic) and
 *          adding a new handler can't silently skip the check. Prefixed with
 *          API_ because they intentionally stay defined for every includer.
 */

#pragma once

#include "security/Auth.hpp"
#include "utils/ErrorResponse.hpp"

/// Reject the request with 403 unless the principal is a full admin.
/// No-op when auth is disabled (AUTH_MODE=none) — same as require_admin.
#define API_REQUIRE_ADMIN(req, callback)                            \
    do {                                                            \
        if (auto _guard_err = Security::Auth::require_admin(req)) { \
            callback(_guard_err);                                   \
            return;                                                 \
        }                                                           \
    } while (0)

/// Reject with 403 unless the principal holds `perm` (a Domain::Permission bit).
/// No-op when auth is disabled. For fine-grained gates beyond full-admin —
/// admins hold every 0xff bit, so they pass automatically.
#define API_REQUIRE_PERMISSION(req, callback, perm)                            \
    do {                                                                       \
        if (auto _guard_err = Security::Auth::require_permission(req, perm)) { \
            callback(_guard_err);                                              \
            return;                                                            \
        }                                                                      \
    } while (0)

/// Resolve the authenticated principal into `var` (std::optional) or
/// reject with 401. Use in handlers that need the caller's identity.
#define API_REQUIRE_PRINCIPAL(req, callback, var)    \
    auto var = Security::Auth::principal_of(req);    \
    do {                                             \
        if (!(var)) {                                \
            callback(ErrorResponse::unauthorized()); \
            return;                                  \
        }                                            \
    } while (0)

/// Bind the caller's user id (principal subject) into `var` (std::string) for
/// owner-scoped resources, or reject with 401. Unlike API_REQUIRE_ADMIN this is
/// NOT a no-op when auth is disabled: a per-user resource is meaningless without
/// an identity, so owner-scoped endpoints genuinely require AUTH_MODE != none.
/// Pass `var` as the owner_id to CrudBase find_owned/list_owned/count_owned.
#define API_REQUIRE_OWNER(req, callback, var)              \
    std::string var;                                       \
    do {                                                   \
        auto _owner_p = Security::Auth::principal_of(req); \
        if (!_owner_p || _owner_p->subject.empty()) {      \
            callback(ErrorResponse::unauthorized());       \
            return;                                        \
        }                                                  \
        (var) = _owner_p->subject;                         \
    } while (0)

/// Reject with 403 unless the caller's email is confirmed (401 if anonymous).
/// No-op when auth is disabled. The confirmed flag is minted into the access
/// JWT but NOT enforced by default — gate your domain's confirmation-required
/// routes with this. flask-base parity: @confirmed_required.
#define API_REQUIRE_CONFIRMED(req, callback)                            \
    do {                                                                \
        if (auto _guard_err = Security::Auth::require_confirmed(req)) { \
            callback(_guard_err);                                       \
            return;                                                     \
        }                                                               \
    } while (0)

/// Reject with 503 when the job queue is disabled. The includer must also
/// include jobs/Jobs.hpp — the macro expands Jobs:: at the use site.
#define API_REQUIRE_JOBS_READY(callback)                                                            \
    do {                                                                                            \
        if (!Jobs::is_initialized()) {                                                              \
            callback(ErrorResponse::service_unavailable("jobs_disabled", "Job queue not enabled")); \
            return;                                                                                 \
        }                                                                                           \
    } while (0)
