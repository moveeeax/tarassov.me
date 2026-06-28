/**
 * @file AccountEmailWorker.hpp
 * @brief Worker-side handler for "account_email" jobs.
 * @details Split from AccountEmails.hpp so HTTP controllers (which only
 *          enqueue) don't pull UserRepository through the email layer.
 *          Included by worker_main.cpp and the integration tests.
 */

#pragma once

#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "email/AccountEmails.hpp"
#include "repositories/UserRepository.hpp"

namespace Email::AccountEmails {

/**
 * @brief Worker-side handler for "account_email" jobs. Reloads the user
 *        (fresh email/name, and the job may be older than an edit),
 *        then delivers. Render/SMTP errors propagate so the job is
 *        retried and eventually DLQ'd; a deleted user is permanent, so
 *        it acks as skipped instead of burning retries.
 */
inline json process_job(const json& payload) {
    const std::string kind = payload.at("kind").get<std::string>();
    const std::string user_id = payload.at("user_id").get<std::string>();

    Repositories::UserRepository repo;
    // Read from the primary: this job often runs moments after the API created
    // the user on the primary, so a lagging replica could spuriously report
    // user_not_found and the worker would permanently skip the email.
    auto user = repo.find(user_id, /*from_primary=*/true);
    if (!user) {
        spdlog::warn("AccountEmails: job for {} skipped — user {} no longer exists", kind, user_id);
        return {{"skipped", "user_not_found"}, {"kind", kind}, {"user_id", user_id}};
    }

    const std::string new_email = payload.value("new_email", "");
    detail::deliver_now(kind, *user, new_email);
    return {{"sent", kind}, {"to", kind == "change_email" ? new_email : user->email}};
}

}  // namespace Email::AccountEmails
