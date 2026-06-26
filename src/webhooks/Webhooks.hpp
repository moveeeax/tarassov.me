/**
 * @file Webhooks.hpp
 * @brief Outbound webhooks: POST an event to a subscriber URL, delivered through
 *        the job system so failures retry with backoff and end in the DLQ
 *        (at-least-once). Bodies are signed with HMAC-SHA256 so receivers can
 *        verify authenticity.
 *
 * This is the DELIVERY seam. WHICH urls receive WHICH events is app-specific —
 * add a subscriptions table + controller (mirror the api_keys feature) and call
 * Webhooks::send() for each subscriber.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "email/Mailer.hpp"  // detail::ensure_curl_init (one-shot curl_global_init)
#include "jobs/Jobs.hpp"
#include "utils/Crypto.hpp"

namespace Webhooks {

using json = nlohmann::json;

inline constexpr const char* kJobType = "webhook.deliver";

namespace detail {

/// Lower-cased host of an http(s) URL, or "" if it isn't one.
inline std::string url_host(const std::string& url) {
    auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return "";
    const std::string proto = url.substr(0, scheme);
    if (proto != "http" && proto != "https")
        return "";
    std::string rest = url.substr(scheme + 3);
    const auto end = rest.find_first_of("/:?#");
    std::string host = end == std::string::npos ? rest : rest.substr(0, end);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    return host;
}

/// Reject obviously-internal targets — a coarse SSRF guard. NOT bullet-proof
/// (no DNS resolution, so it can't stop DNS-rebinding or a public name pointing
/// at a private A record); pair it with an egress allow-list in real prod.
inline bool host_is_blocked(const std::string& host) {
    if (host.empty() || host == "localhost")
        return true;
    if (host.rfind("127.", 0) == 0 || host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0 ||
        host.rfind("169.254.", 0) == 0 || host == "::1" || host == "[::1]")
        return true;
    // 172.16.0.0/12
    if (host.rfind("172.", 0) == 0) {
        const auto dot = host.find('.', 4);
        if (dot != std::string::npos) {
            const int second = std::atoi(host.substr(4, dot - 4).c_str());
            if (second >= 16 && second <= 31)
                return true;
        }
    }
    return false;
}

}  // namespace detail

/**
 * @brief Worker-side: deliver one webhook. THROWS on a blocked URL, transport
 *        error, or non-2xx response so Jobs drives retry/backoff/DLQ.
 */
inline json process_job(const json& payload) {
    const std::string url = payload.value("url", "");
    const std::string event = payload.value("event", "");
    const std::string body = payload.contains("body") ? payload["body"].dump() : "{}";
    const std::string secret = payload.value("secret", "");

    const std::string host = detail::url_host(url);
    if (host.empty())
        throw std::runtime_error("webhook: url is not http(s): " + url);
    if (detail::host_is_blocked(host))
        throw std::runtime_error("webhook: refusing to POST to internal host: " + host);

    Email::detail::ensure_curl_init();
    std::unique_ptr<CURL, void (*)(CURL*)> h(curl_easy_init(), curl_easy_cleanup);
    if (!h)
        throw std::runtime_error("webhook: curl_easy_init failed");

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    const std::string event_hdr = "X-Webhook-Event: " + event;
    hdrs = curl_slist_append(hdrs, event_hdr.c_str());
    std::string sig_hdr;
    if (!secret.empty()) {
        const std::string raw = Utils::Crypto::hmac_sha256(secret, body);
        const std::string hex =
            Utils::Crypto::detail::bytes_to_hex(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
        sig_hdr = "X-Webhook-Signature: sha256=" + hex;
        hdrs = curl_slist_append(hdrs, sig_hdr.c_str());
    }
    std::unique_ptr<curl_slist, void (*)(curl_slist*)> hdr_guard(hdrs, curl_slist_free_all);

    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(h.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(h.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 0L);  // a redirect could bypass the SSRF check
    curl_easy_setopt(
        h.get(), CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });

    const CURLcode rc = curl_easy_perform(h.get());
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("webhook: transport error: ") + curl_easy_strerror(rc));
    long status = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300)
        throw std::runtime_error("webhook: non-2xx response (" + std::to_string(status) + ") from " + host);

    return json{{"delivered", true}, {"status", status}, {"event", event}};
}

/**
 * @brief App-facing: enqueue a webhook for delivery. Best-effort — never throws.
 *        Webhooks REQUIRE the job queue (async retry/DLQ); if Jobs is down the
 *        event is dropped with a warning rather than blocking the caller.
 */
inline void send(const std::string& url, const std::string& event, const json& body, const std::string& secret = "") {
    if (!Jobs::is_initialized()) {
        spdlog::warn("webhook: jobs not initialized; dropping {} -> {}", event, url);
        return;
    }
    try {
        json payload = {{"url", url}, {"event", event}, {"body", body}};
        if (!secret.empty())
            payload["secret"] = secret;
        Jobs::get().submit(kJobType, payload);
    } catch (const std::exception& e) {
        spdlog::warn("webhook: enqueue {} -> {} failed ({})", event, url, e.what());
    }
}

}  // namespace Webhooks
