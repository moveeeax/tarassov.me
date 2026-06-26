/**
 * @file test_module_guards.cpp
 * @brief Unit tests for the lifecycle/guard contracts of modules that have
 *        no other direct coverage: Messaging, Tasks, and the SqlErrors
 *        translate_sql wrapper. Pure — no Kafka broker, no Postgres.
 */

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "messaging/Messaging.hpp"
#include "repositories/SqlErrors.hpp"
#include "tasks/Tasks.hpp"

namespace {

// ---- Messaging lifecycle (no broker needed: ctor doesn't connect) ----------

TEST(MessagingGuardTest, GetBeforeInitThrows) {
    if (Messaging::is_initialized())
        Messaging::shutdown();
    EXPECT_FALSE(Messaging::is_initialized());
    EXPECT_THROW(Messaging::get(), std::runtime_error);
}

TEST(MessagingGuardTest, InitThrowsOnDoubleInitAndShutdownResets) {
    if (Messaging::is_initialized())
        Messaging::shutdown();
    Messaging::initialize();
    EXPECT_TRUE(Messaging::is_initialized());
    EXPECT_NO_THROW(Messaging::get());
    // Messaging follows the throw-on-reinit convention (like Cache/Jobs/
    // Database), NOT the warned-no-op one (Auth/RateLimit/Idempotency).
    EXPECT_THROW(Messaging::initialize(), std::runtime_error);
    EXPECT_TRUE(Messaging::is_initialized());
    Messaging::shutdown();
    EXPECT_FALSE(Messaging::is_initialized());
    EXPECT_THROW(Messaging::get(), std::runtime_error);
}

// ---- Tasks guards ----------------------------------------------------------

TEST(TasksGuardTest, ScheduleBeforeInitThrows) {
    if (Tasks::is_initialized())
        Tasks::shutdown();
    EXPECT_THROW(Tasks::schedule_recurring("t", std::chrono::milliseconds(1000), [] {}), std::runtime_error);
}

TEST(TasksGuardTest, CancelUnknownReturnsFalse) {
    // cancel() doesn't require init and must not touch the event loop for a
    // miss — safe to call in a unit test.
    EXPECT_FALSE(Tasks::cancel("does-not-exist"));
}

// ---- SqlErrors::translate_sql ----------------------------------------------

TEST(SqlErrorsTest, ReturnsBodyResultWhenNoError) {
    int calls = 0;
    auto translator = [&](std::string_view) { ++calls; };  // must NOT be called
    int r = Repositories::detail::translate_sql([] { return 42; }, translator);
    EXPECT_EQ(r, 42);
    EXPECT_EQ(calls, 0);
}

TEST(SqlErrorsTest, NonSqlErrorPropagatesUnchangedAndSkipsTranslator) {
    int calls = 0;
    auto translator = [&](std::string_view) { ++calls; };
    EXPECT_THROW(Repositories::detail::translate_sql(
                     [] {
                         throw std::runtime_error("not a sql_error");
                         return 0;
                     },
                     translator),
                 std::runtime_error);
    EXPECT_EQ(calls, 0) << "translator must only fire on pqxx::sql_error";
}

}  // namespace
