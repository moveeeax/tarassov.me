/**
 * @file test_webhooks.cpp
 * @brief Webhook URL guard — the SSRF defense. No infra (no HTTP is made:
 *        rejection happens before any network call).
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "webhooks/Webhooks.hpp"

using json = nlohmann::json;

namespace {

TEST(Webhooks, BlocksInternalAndMetadataHosts) {
    using Webhooks::detail::host_is_blocked;
    using Webhooks::detail::url_host;

    EXPECT_TRUE(host_is_blocked(url_host("http://localhost/hook")));
    EXPECT_TRUE(host_is_blocked(url_host("http://127.0.0.1:8080/hook")));
    EXPECT_TRUE(host_is_blocked(url_host("https://10.0.0.5/hook")));
    EXPECT_TRUE(host_is_blocked(url_host("https://192.168.1.1/")));
    EXPECT_TRUE(host_is_blocked(url_host("http://172.16.0.1/")));
    EXPECT_TRUE(host_is_blocked(url_host("http://172.31.255.255/")));
    // The cloud metadata endpoint — the classic SSRF target.
    EXPECT_TRUE(host_is_blocked(url_host("http://169.254.169.254/latest/meta-data/")));

    EXPECT_FALSE(host_is_blocked(url_host("https://hooks.example.com/x")));
    EXPECT_FALSE(host_is_blocked(url_host("https://172.32.0.1/")));  // just outside 172.16/12

    EXPECT_TRUE(url_host("ftp://internal/x").empty());  // non-http(s) → no host
    EXPECT_TRUE(url_host("not a url").empty());
}

TEST(Webhooks, ProcessJobRefusesInternalOrNonHttpUrl) {
    EXPECT_THROW(Webhooks::process_job({{"url", "http://169.254.169.254/"}, {"event", "e"}, {"body", json::object()}}),
                 std::exception);
    EXPECT_THROW(Webhooks::process_job({{"url", "ftp://nope/"}, {"event", "e"}}), std::exception);
    EXPECT_THROW(Webhooks::process_job({{"url", ""}, {"event", "e"}}), std::exception);
}

}  // namespace
