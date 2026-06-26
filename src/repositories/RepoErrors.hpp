/**
 * @file RepoErrors.hpp
 * @brief Generic repository-error base classes that carry their own HTTP
 *        mapping, with NO dependency on any concrete domain (no pqxx, no
 *        Database, no User/Role).
 *
 * The shared controller helper Api::with_repo_errors() catches THESE bases,
 * not the concrete domain exceptions, so the api-side plumbing stays decoupled
 * from the demo auth domain: delete User/Role and the helper still compiles,
 * and a forked repository that throws its own NotFoundError/ConflictError
 * subclass is mapped to the right status code automatically — no edit to the
 * shared handler.
 *
 * Concrete domain exceptions (DuplicateEmail, UserNotFound, …) live next to
 * their repository and derive from these bases, passing their stable machine
 * code / resource token.
 */

#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace Repositories {

/**
 * @brief Base for EXPECTED domain errors that map to a specific 4xx — as
 *        opposed to an unexpected std::exception, which maps to 500.
 */
struct RepoError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief → 404. @p resource is the machine token placed in the error body
 *        ("user", "role").
 */
struct NotFoundError : RepoError {
    explicit NotFoundError(std::string resource)
        : RepoError("not found: " + resource), resource_(std::move(resource)) {}
    const std::string& resource() const noexcept { return resource_; }

private:
    std::string resource_;
};

/**
 * @brief → 409. @p code is the stable machine code ("email_taken");
 *        @p message is the human-readable detail.
 */
struct ConflictError : RepoError {
    ConflictError(std::string code, std::string message)
        : RepoError(message.empty() ? code : message), code_(std::move(code)), message_(std::move(message)) {}
    const std::string& code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

private:
    std::string code_;
    std::string message_;
};

}  // namespace Repositories
