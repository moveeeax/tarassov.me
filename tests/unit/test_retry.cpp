#include <atomic>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "utils/Retry.hpp"

namespace {

struct TransientError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct PermanentError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool is_transient(const std::exception& e) {
    return dynamic_cast<const TransientError*>(&e) != nullptr;
}

Retry::Policy fast_policy() {
    Retry::Policy p;
    p.max_attempts = 3;
    p.base_delay_ms = 1;
    p.max_delay_ms = 5;
    p.jitter = false;
    return p;
}

}  // namespace

TEST(RetryTest, SuccessFirstTry) {
    std::atomic<int> calls{0};
    auto result = Retry::run(
        [&] {
            calls++;
            return 42;
        },
        is_transient,
        fast_policy(),
        "t");
    EXPECT_EQ(result, 42);
    EXPECT_EQ(calls.load(), 1);
}

TEST(RetryTest, RetriesUntilSuccess) {
    std::atomic<int> calls{0};
    auto result = Retry::run(
        [&] {
            if (++calls < 3)
                throw TransientError("boom");
            return 99;
        },
        is_transient,
        fast_policy(),
        "t");
    EXPECT_EQ(result, 99);
    EXPECT_EQ(calls.load(), 3);
}

TEST(RetryTest, GivesUpAfterMaxAttempts) {
    std::atomic<int> calls{0};
    EXPECT_THROW(
        {
            Retry::run(
                [&] {
                    ++calls;
                    throw TransientError("always");
                    return 0;
                },
                is_transient,
                fast_policy(),
                "t");
        },
        TransientError);
    EXPECT_EQ(calls.load(), 3);  // max_attempts
}

TEST(RetryTest, PermanentErrorNotRetried) {
    std::atomic<int> calls{0};
    EXPECT_THROW(
        {
            Retry::run(
                [&] {
                    ++calls;
                    throw PermanentError("nope");
                    return 0;
                },
                is_transient,
                fast_policy(),
                "t");
        },
        PermanentError);
    EXPECT_EQ(calls.load(), 1);
}

TEST(RetryTest, VoidReturnType) {
    std::atomic<int> calls{0};
    Retry::run([&] { calls++; }, is_transient, fast_policy(), "t");
    EXPECT_EQ(calls.load(), 1);
}

TEST(RetryTest, RetriesExhaustAndFailFast) {
    // Smoke test ONLY: asserts the retry loop terminates promptly with tiny
    // delays and performs exactly max_attempts calls. It does NOT verify the
    // backoff cap itself — that would need an injectable sleep hook; the
    // wall-clock bound below is a loose guard against a hung loop, not a
    // delay-cap assertion.
    auto start = std::chrono::steady_clock::now();
    std::atomic<int> calls{0};
    EXPECT_THROW(
        {
            Retry::Policy p;
            p.max_attempts = 3;
            p.base_delay_ms = 1;
            p.max_delay_ms = 2;
            p.jitter = false;
            Retry::run(
                [&] {
                    ++calls;
                    throw TransientError("x");
                    return 0;
                },
                is_transient,
                p,
                "t");
        },
        TransientError);
    auto elapsed = std::chrono::steady_clock::now() - start;
    // 2 retries with delays ≤ max(2ms) — should be well under 100ms even
    // under CI load.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_EQ(calls.load(), 3);
}
