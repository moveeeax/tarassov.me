/**
 * @file test_job_dispatch.cpp
 * @brief Unit tests for the header-only Jobs::Dispatcher — the job-type→handler
 *        registry extracted from worker_main.cpp. Pure logic: no Postgres/Redis,
 *        so it lives in the unit bucket and exercises the dispatch + unknown-type
 *        path that the old in-.cpp if-ladder left untestable.
 */

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "jobs/BuiltinHandlers.hpp"
#include "jobs/Dispatcher.hpp"
#include "jobs/Job.hpp"

using nlohmann::json;

namespace {
Jobs::Job make_job(const std::string& type, json payload) {
    Jobs::Job j;
    j.type = type;
    j.payload = std::move(payload);
    return j;
}
}  // namespace

TEST(JobDispatchTest, DispatchesRegisteredHandler) {
    auto& d = Jobs::Dispatcher::get();
    d.register_handler("dispatch_echo_test", [](const json& p) { return p; });
    ASSERT_TRUE(d.has_handler("dispatch_echo_test"));
    auto out = d.dispatch(make_job("dispatch_echo_test", {{"x", 42}}));
    EXPECT_EQ(out["x"].get<int>(), 42);
}

TEST(JobDispatchTest, UnknownTypeThrowsPermanentJobError) {
    auto& d = Jobs::Dispatcher::get();
    // A type nobody registered must be a PERMANENT failure (→ straight to DLQ),
    // not a silent success and not a retry-storm.
    EXPECT_THROW(d.dispatch(make_job("no_such_job_type_zzz", json::object())), Jobs::PermanentJobError);
}

TEST(JobDispatchTest, HandlerExceptionPropagates) {
    auto& d = Jobs::Dispatcher::get();
    d.register_handler("dispatch_boom_test", [](const json&) -> json { throw std::runtime_error("boom"); });
    EXPECT_THROW(d.dispatch(make_job("dispatch_boom_test", json::object())), std::runtime_error);
}

TEST(JobDispatchTest, KnownTypesReflectsRegistration) {
    auto& d = Jobs::Dispatcher::get();
    d.register_handler("dispatch_probe_test", [](const json&) { return json::object(); });
    auto types = d.known_types();
    EXPECT_NE(std::find(types.begin(), types.end(), "dispatch_probe_test"), types.end());
}

TEST(JobDispatchTest, UnregisteredReportsTypesWithoutHandlers) {
    auto& d = Jobs::Dispatcher::get();
    d.register_handler("dispatch_known_test", [](const json&) { return json::object(); });
    // A WORKER_TYPES entry with no handler must be surfaced (it would silently
    // dead-letter every job of that type); registered types are filtered out.
    auto missing = d.unregistered({"dispatch_known_test", "dispatch_absent_test_zzz"});
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "dispatch_absent_test_zzz");
    EXPECT_TRUE(d.unregistered({"dispatch_known_test"}).empty());
}

TEST(JobDispatchTest, BuiltinHandlersAreRegistered) {
    // Guards the if-ladder→Dispatcher refactor: dropping/renaming a built-in
    // (esp. account_email) would silently dead-letter real jobs with no other
    // failing test. register_builtin_handlers() lives in a header for exactly
    // this reason.
    Jobs::register_builtin_handlers();
    auto& d = Jobs::Dispatcher::get();
    EXPECT_TRUE(d.has_handler(Email::AccountEmails::kJobType));
    EXPECT_TRUE(d.has_handler("echo"));
    EXPECT_TRUE(d.has_handler("slow"));
    EXPECT_TRUE(d.has_handler("fail"));
}
