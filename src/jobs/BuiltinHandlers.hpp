/**
 * @file BuiltinHandlers.hpp
 * @brief Registers the job handlers the template ships with into the global
 *        Jobs::Dispatcher. Extracted from worker_main.cpp so the registration
 *        (e.g. "is account_email actually registered?") is unit-testable — the
 *        if-ladder→Dispatcher refactor could otherwise silently drop a handler
 *        and only a live worker run would notice.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "email/AccountEmailWorker.hpp"
#include "email/GenericEmail.hpp"
#include "jobs/Dispatcher.hpp"
#include "webhooks/Webhooks.hpp"

namespace Jobs {

using json = nlohmann::json;

/**
 * @brief Register the built-in handlers. Call once at worker startup. Add a new
 *        built-in here, or have a handler header self-register via
 *        Jobs::JobHandlerRegistrar and just #include it from worker_main.cpp.
 */
inline void register_builtin_handlers() {
    auto& d = Dispatcher::get();
    // Account emails (confirm / reset / change-email / invite). Throws on
    // render/SMTP failure → retried, then DLQ'd.
    d.register_handler(Email::AccountEmails::kJobType,
                       [](const json& payload) { return Email::AccountEmails::process_job(payload); });
    // Generic ad-hoc email for any app code (not tied to account flows). Same
    // throw-on-failure → retry/DLQ contract.
    d.register_handler(Email::SendEmail::kJobType,
                       [](const json& payload) { return Email::SendEmail::process_job(payload); });
    // Outbound webhooks: signed POST to a subscriber URL, same retry/DLQ contract.
    d.register_handler(Webhooks::kJobType, [](const json& payload) { return Webhooks::process_job(payload); });
    // Demo handlers used by examples/tests.
    d.register_handler("echo", [](const json& payload) { return payload; });
    d.register_handler("slow", [](const json& payload) -> json {
        int seconds = 2;
        if (payload.contains("seconds") && payload["seconds"].is_number())
            seconds = std::max(1, std::min(payload["seconds"].get<int>(), 30));
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        return {{"slept_seconds", seconds}, {"message", "done"}};
    });
    d.register_handler("fail",
                       [](const json&) -> json { throw std::runtime_error("intentional failure for testing"); });
}

}  // namespace Jobs
