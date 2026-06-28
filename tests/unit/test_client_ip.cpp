/**
 * @file test_client_ip.cpp
 * @brief RateLimit::client_ip trusted-IP resolution (anti-spoofing). No infra.
 */

#include <string>
#include <utility>
#include <vector>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include "security/RateLimit.hpp"

using namespace drogon;

namespace {

HttpRequestPtr req_with(const std::vector<std::pair<std::string, std::string>>& headers) {
    auto r = HttpRequest::newHttpRequest();
    for (const auto& [k, v] : headers)
        r->addHeader(k, v);
    return r;
}

// trust_proxy=false: client-supplied headers are NEVER trusted (spoofable) —
// fall back to the peer address.
TEST(ClientIp, UntrustedIgnoresForwardHeaders) {
    auto r = req_with({{"X-Real-IP", "1.2.3.4"}, {"X-Forwarded-For", "9.9.9.9"}});
    EXPECT_NE(Security::RateLimit::client_ip(r, /*trust_proxy=*/false), "1.2.3.4");
    EXPECT_NE(Security::RateLimit::client_ip(r, /*trust_proxy=*/false), "9.9.9.9");
}

// trust_proxy=true: X-Real-IP (set by our nginx) wins over XFF.
TEST(ClientIp, TrustedPrefersXRealIp) {
    auto r = req_with({{"X-Real-IP", "1.2.3.4"}, {"X-Forwarded-For", "9.9.9.9, 8.8.8.8"}});
    EXPECT_EQ(Security::RateLimit::client_ip(r, /*trust_proxy=*/true), "1.2.3.4");
}

// XFF only: the real client is trusted_proxy_count hops from the right.
TEST(ClientIp, TrustedXffByHopCount) {
    auto r = req_with({{"X-Forwarded-For", "203.0.113.7, 10.0.0.1, 10.0.0.2"}});
    EXPECT_EQ(Security::RateLimit::client_ip(r, /*trust_proxy=*/true, /*count=*/1), "10.0.0.2");
    EXPECT_EQ(Security::RateLimit::client_ip(r, /*trust_proxy=*/true, /*count=*/3), "203.0.113.7");
}

// The config-driven convenience: with no app Config initialized it must default
// to UNTRUSTED (never echo a spoofable header) — the safe default for the audit.
TEST(ClientIp, ConvenienceDefaultsUntrusted) {
    auto r = req_with({{"X-Real-IP", "1.2.3.4"}});
    EXPECT_NE(Security::RateLimit::client_ip(r), "1.2.3.4");
}

}  // namespace
