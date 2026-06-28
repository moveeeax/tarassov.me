/**
 * @file test_handler_support.cpp
 * @brief Unit tests for Api::with_repo_errors — the shared repository-exception
 *        → HTTP mapping.
 *
 * The key property under test is DECOUPLING: the helper catches the generic
 * Repositories::NotFoundError / ConflictError bases, so a repository exception
 * it has never heard of — only deriving from those bases — still maps to the
 * right status without any edit to HandlerSupport. A forked domain that deletes
 * User/Role keeps working.
 */

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/HandlerSupport.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/UserRepository.hpp"

using json = nlohmann::json;

namespace {

struct Captured {
    bool returned = false;   // with_repo_errors() return value
    bool responded = false;  // callback fired?
    int status = 0;
    std::string error;
    std::string message;
};

template <typename Fn>
Captured run(Fn&& fn) {
    Captured cap;
    cap.returned = Api::with_repo_errors(
        [&](const drogon::HttpResponsePtr& resp) {
            cap.responded = true;
            cap.status = static_cast<int>(resp->statusCode());
            auto body = json::parse(resp->body());
            cap.error = body.value("error", "");
            cap.message = body.value("message", "");
        },
        "unit test",
        std::forward<Fn>(fn));
    return cap;
}

// A domain the helper has NEVER heard of — stands in for a forker's repository.
struct WidgetExists : Repositories::ConflictError {
    WidgetExists() : ConflictError("widget_exists", "That widget already exists") {}
};
struct WidgetNotFound : Repositories::NotFoundError {
    WidgetNotFound() : NotFoundError("widget") {}
};

TEST(WithRepoErrors, NoThrowReturnsTrueAndDoesNotRespond) {
    auto cap = run([] {});
    EXPECT_TRUE(cap.returned);
    EXPECT_FALSE(cap.responded);
}

TEST(WithRepoErrors, DuplicateEmailMaps409EmailTaken) {
    auto cap = run([] { throw Repositories::DuplicateEmail{}; });
    EXPECT_FALSE(cap.returned);
    EXPECT_EQ(cap.status, 409);
    EXPECT_EQ(cap.error, "email_taken");
}

TEST(WithRepoErrors, UserNotFoundMaps404) {
    auto cap = run([] { throw Repositories::UserNotFound{}; });
    EXPECT_EQ(cap.status, 404);
    EXPECT_EQ(cap.error, "not_found");
    EXPECT_NE(cap.message.find("user"), std::string::npos);
}

// The decoupling guarantee: an exception type HandlerSupport doesn't know,
// mapped purely through the ConflictError base.
TEST(WithRepoErrors, UnknownDomainConflictMapsByBase) {
    auto cap = run([] { throw WidgetExists{}; });
    EXPECT_EQ(cap.status, 409);
    EXPECT_EQ(cap.error, "widget_exists");
}

TEST(WithRepoErrors, UnknownDomainNotFoundMapsByBase) {
    auto cap = run([] { throw WidgetNotFound{}; });
    EXPECT_EQ(cap.status, 404);
    EXPECT_NE(cap.message.find("widget"), std::string::npos);
}

TEST(WithRepoErrors, UnexpectedExceptionMaps500) {
    auto cap = run([] { throw std::runtime_error("boom"); });
    EXPECT_EQ(cap.status, 500);
}

}  // namespace
