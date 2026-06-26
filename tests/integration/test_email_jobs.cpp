/**
 * @file test_email_jobs.cpp
 * @brief Integration tests for the email-via-jobs routing.
 *
 * With Jobs enabled, AccountEmails::send_*() must enqueue an
 * "account_email" job instead of touching SMTP from the request thread;
 * the worker-side handler (AccountEmails::process_job) does the actual
 * token + render + send. Mail stays disabled here, so delivery is the
 * Mailer's logged no-op — what's under test is the routing and the
 * handler's contract (ack, skip, throw).
 *
 * Skips when Postgres / Redis are not reachable.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "email/AccountEmailWorker.hpp"
#include "email/GenericEmail.hpp"
#include "jobs/Jobs.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class EmailJobsTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "email_jobs_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = "test-jwt-secret-for-email-jobs-padding";
        cfg["jobs"]["enabled"] = true;
        cfg["mail"]["enabled"] = false;  // delivery = logged no-op
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;

        TestHelpers::truncate_users();
        drain_queue();
    }

    void TearDown() override {
        if (!::testing::Test::IsSkipped() && Cache::is_initialized())
            drain_queue();
        TestHelpers::CoreBackedTest::TearDown();
    }

    /// Shared cleanup: blobs + queue/dlq/index keys for the email job types.
    static void drain_queue() { TestHelpers::drain_jobs({Email::AccountEmails::kJobType, Email::SendEmail::kJobType}); }

    Domain::User make_user(const std::string& email) {
        Repositories::RoleRepository roles;
        auto role = roles.find_default();
        Repositories::UserRepository repo;
        auto user = repo.create(email, std::nullopt, std::nullopt, std::nullopt, role->id);
        user.role = *role;
        return user;
    }

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(Email::AccountEmails::kJobType)));
    }
};

TEST_F(EmailJobsTest, sendConfirmEnqueuesInsteadOfSendingInline) {
    auto user = make_user("queued@example.com");

    Email::AccountEmails::send_confirm(user);

    ASSERT_EQ(queue_depth(), 1);
    auto& redis = Cache::get().get_client();
    std::vector<std::string> ids;
    redis.lrange(Jobs::queue_key(Email::AccountEmails::kJobType), 0, -1, std::back_inserter(ids));
    auto job = Jobs::get().get_status(ids[0]);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->type, Email::AccountEmails::kJobType);
    EXPECT_EQ(job->status, "pending");
    EXPECT_EQ(job->payload["kind"], "confirm");
    EXPECT_EQ(job->payload["user_id"], user.id);
}

TEST_F(EmailJobsTest, changeEmailPayloadCarriesNewAddress) {
    auto user = make_user("old@example.com");

    Email::AccountEmails::send_change_email(user, "new@example.com");

    std::vector<std::string> ids;
    Cache::get().get_client().lrange(Jobs::queue_key(Email::AccountEmails::kJobType), 0, -1, std::back_inserter(ids));
    ASSERT_EQ(ids.size(), 1u);
    auto job = Jobs::get().get_status(ids[0]);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->payload["kind"], "change_email");
    EXPECT_EQ(job->payload["new_email"], "new@example.com");
}

TEST_F(EmailJobsTest, processJobDeliversAndAcks) {
    auto user = make_user("handler@example.com");
    json payload = {{"kind", "confirm"}, {"user_id", user.id}};

    auto result = Email::AccountEmails::process_job(payload);

    EXPECT_EQ(result["sent"], "confirm");
    EXPECT_EQ(result["to"], "handler@example.com");
}

TEST_F(EmailJobsTest, processJobSkipsDeletedUserWithoutThrowing) {
    json payload = {{"kind", "confirm"}, {"user_id", "00000000-0000-0000-0000-000000000000"}};

    auto result = Email::AccountEmails::process_job(payload);

    EXPECT_EQ(result["skipped"], "user_not_found");
}

TEST_F(EmailJobsTest, processJobThrowsOnUnknownKind) {
    auto user = make_user("badkind@example.com");
    json payload = {{"kind", "newsletter"}, {"user_id", user.id}};

    EXPECT_THROW(Email::AccountEmails::process_job(payload), std::runtime_error);
}

TEST_F(EmailJobsTest, viaJobsFalseSendsInline) {
    // Flip the runtime toggle off via env (env wins over JSON config).
    setenv("MAIL_VIA_JOBS", "false", 1);
    auto user = make_user("inline@example.com");

    Email::AccountEmails::send_confirm(user);
    unsetenv("MAIL_VIA_JOBS");

    // Inline path: nothing enqueued; delivery itself is the disabled
    // mailer's logged no-op.
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(EmailJobsTest, genericSendEmailEnqueuesWithPayload) {
    Email::SendEmail::send("ad-hoc@example.com", "Hello", "plain body", "<b>html</b>");

    auto& redis = Cache::get().get_client();
    std::vector<std::string> ids;
    redis.lrange(Jobs::queue_key(Email::SendEmail::kJobType), 0, -1, std::back_inserter(ids));
    ASSERT_EQ(ids.size(), 1u);
    auto job = Jobs::get().get_status(ids[0]);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->type, Email::SendEmail::kJobType);
    EXPECT_EQ(job->payload["to"], "ad-hoc@example.com");
    EXPECT_EQ(job->payload["subject"], "Hello");
    EXPECT_EQ(job->payload["text"], "plain body");
    EXPECT_EQ(job->payload["html"], "<b>html</b>");
}

TEST_F(EmailJobsTest, genericSendEmailRejectsMissingFields) {
    // Worker-side validation: no recipient, or no body, must throw so the job
    // is retried/DLQ'd rather than silently "succeeding".
    EXPECT_THROW(Email::SendEmail::process_job({{"subject", "x"}, {"text", "y"}}), std::exception);
    EXPECT_THROW(Email::SendEmail::process_job({{"to", "x@example.com"}, {"subject", "x"}}), std::exception);
}

}  // namespace
