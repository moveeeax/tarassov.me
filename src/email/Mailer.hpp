/**
 * @file Mailer.hpp
 * @brief SMTP outbound mail via libcurl.
 *
 * flask-base parity: app/email.py uses Flask-Mail to push messages onto
 * Flask-RQ. We do the same shape: Mailer::send() is synchronous and
 * cheap to call from a job worker; the controller submits a job rather
 * than blocking the request thread on SMTP I/O.
 *
 * Single-process global singleton like every other module in this
 * template. Off by default (`mail.enabled=false`) — the controllers
 * fall back to logging the link until SMTP is configured. In dev-compose
 * we point at Mailpit (smtp://mailpit:1025), no TLS / no auth.
 *
 * Config keys:
 *   mail.enabled              bool   default false
 *   mail.smtp_host            string default localhost
 *   mail.smtp_port            int    default 1025  (Mailpit dev)
 *   mail.smtp_username        string default ""    (anonymous)
 *   mail.smtp_password        string default ""
 *   mail.smtp_use_tls         bool   default false (STARTTLS for prod)
 *   mail.from                 string default "noreply@example.com"
 *   mail.from_name            string default APP_NAME
 *   mail.subject_prefix       string default "[App] "
 *   mail.timeout_sec          int    default 30
 */

#pragma once

#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "utils/Config.hpp"
#include "utils/Strings.hpp"

namespace Email {

struct MailerConfig {
    bool enabled = false;
    std::string smtp_host = "localhost";
    int smtp_port = 1025;
    std::string smtp_username;
    std::string smtp_password;
    bool use_tls = false;
    std::string from = "noreply@example.com";
    std::string from_name = "App";
    std::string subject_prefix = "[App] ";
    int timeout_sec = 30;
};

struct Message {
    std::string to;         // single recipient — flask-base stays single-recipient too
    std::string subject;    // prefix is added by Mailer; pass the bare subject
    std::string text_body;  // plain text alt
    std::string html_body;  // HTML body
};

namespace detail {

/**
 * @brief One-shot global libcurl init. CURL needs `curl_global_init` once
 *        per process before any `curl_easy_*` calls. Sodium-style
 *        idempotent guard.
 */
inline void ensure_curl_init() {
    static const CURLcode rc = ::curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl_global_init failed: ") + curl_easy_strerror(rc));
}

inline std::string format_rfc5322_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm);
    return buf;
}

inline std::string make_message_id(const std::string& host) {
    static thread_local std::mt19937_64 rng{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::ostringstream oss;
    oss << "<" << std::hex << rng() << "@" << (host.empty() ? "localhost" : host) << ">";
    return oss.str();
}

/**
 * @brief Build a multipart/alternative MIME blob. Single recipient, single
 *        text + html part. Boundary is random per message — enough for
 *        the simple parity case; not a full MIME library.
 */
// Strip CR/LF so a value can't inject extra SMTP headers (header injection).
// Defense-in-depth: the recipient address is also email-validated upstream,
// but the subject is free-form.
inline std::string strip_crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r' && c != '\n')
            out += c;
    return out;
}

inline std::string build_mime(const MailerConfig& cfg, const Message& msg, const std::string& message_id) {
    static thread_local std::mt19937_64 rng{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::ostringstream b;
    b << "----=_Part_" << std::hex << rng();
    const std::string boundary = b.str();

    const std::string to = strip_crlf(msg.to);
    const std::string subject = strip_crlf(msg.subject);

    std::ostringstream out;
    out << "Date: " << format_rfc5322_date() << "\r\n";
    out << "From: ";
    if (!cfg.from_name.empty())
        out << "\"" << cfg.from_name << "\" <" << cfg.from << ">\r\n";
    else
        out << cfg.from << "\r\n";
    out << "To: " << to << "\r\n";
    // Join prefix and subject with a space unless the prefix already ends
    // with one — env-file values tend to lose trailing whitespace.
    out << "Subject: " << cfg.subject_prefix;
    if (!cfg.subject_prefix.empty() && cfg.subject_prefix.back() != ' ')
        out << ' ';
    out << subject << "\r\n";
    out << "Message-ID: " << message_id << "\r\n";
    out << "MIME-Version: 1.0\r\n";
    out << "Content-Type: multipart/alternative; boundary=\"" << boundary << "\"\r\n";
    out << "\r\n";
    out << "This is a MIME message.\r\n";

    if (!msg.text_body.empty()) {
        out << "--" << boundary << "\r\n";
        out << "Content-Type: text/plain; charset=utf-8\r\n";
        out << "Content-Transfer-Encoding: 8bit\r\n";
        out << "\r\n" << msg.text_body << "\r\n";
    }
    if (!msg.html_body.empty()) {
        out << "--" << boundary << "\r\n";
        out << "Content-Type: text/html; charset=utf-8\r\n";
        out << "Content-Transfer-Encoding: 8bit\r\n";
        out << "\r\n" << msg.html_body << "\r\n";
    }
    out << "--" << boundary << "--\r\n";
    return out.str();
}

/**
 * @brief Read-callback for libcurl's CURLOPT_READFUNCTION. The userdata
 *        is a pair (pointer to body, current cursor) — we feed bytes
 *        out chunk by chunk.
 */
struct ReadCtx {
    const std::string* body;
    size_t pos;
};

inline size_t read_cb(char* dest, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<ReadCtx*>(userp);
    const size_t avail = ctx->body->size() - ctx->pos;
    const size_t want = size * nmemb;
    const size_t take = avail < want ? avail : want;
    if (take == 0)
        return 0;
    std::memcpy(dest, ctx->body->data() + ctx->pos, take);
    ctx->pos += take;
    return take;
}

}  // namespace detail

class Mailer {
public:
    explicit Mailer(MailerConfig cfg) : config_(std::move(cfg)) { detail::ensure_curl_init(); }

    const MailerConfig& config() const { return config_; }

    /**
     * @brief Send a single message. Returns true on success.
     *        On failure, logs the error and returns false — the caller
     *        (a job worker, usually) decides whether to retry.
     *
     *        When the mailer is disabled, returns true after logging the
     *        message at INFO level. This is the dev-loop shortcut: the
     *        confirm-email link still ends up in app logs even with no
     *        SMTP configured.
     */
    bool send(const Message& msg) {
        if (!config_.enabled) {
            spdlog::info("[mailer disabled] would send to={} subject='{}' body_len={}",
                         Utils::Strings::mask_email(msg.to),
                         msg.subject,
                         msg.text_body.size());
            return true;
        }
        if (msg.to.empty() || msg.subject.empty() || (msg.text_body.empty() && msg.html_body.empty())) {
            spdlog::error("Mailer::send: missing required field(s)");
            return false;
        }

        // No lock: the easy handle is created per-call below, so there's no
        // shared CURL state to serialize. Holding a process-wide mutex here
        // would queue every outbound email behind one SMTP round-trip
        // (up to timeout_sec each).
        std::unique_ptr<CURL, void (*)(CURL*)> handle(curl_easy_init(), curl_easy_cleanup);
        if (!handle)
            return false;

        // smtps:// for implicit TLS, smtp:// for plain or STARTTLS.
        std::string url = (config_.use_tls && config_.smtp_port == 465 ? "smtps://" : "smtp://") + config_.smtp_host +
                          ":" + std::to_string(config_.smtp_port);
        curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());

        if (!config_.smtp_username.empty()) {
            curl_easy_setopt(handle.get(), CURLOPT_USERNAME, config_.smtp_username.c_str());
            curl_easy_setopt(handle.get(), CURLOPT_PASSWORD, config_.smtp_password.c_str());
        }

        // STARTTLS for port-587 style. CURLUSESSL_ALL would also fail
        // when the server doesn't speak TLS — useful in prod.
        if (config_.use_tls && config_.smtp_port != 465) {
            curl_easy_setopt(handle.get(), CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        }

        const std::string from_addr = "<" + config_.from + ">";
        curl_easy_setopt(handle.get(), CURLOPT_MAIL_FROM, from_addr.c_str());

        struct curl_slist* recipients = nullptr;
        const std::string rcpt = "<" + msg.to + ">";
        recipients = curl_slist_append(recipients, rcpt.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_MAIL_RCPT, recipients);

        const std::string message_id = detail::make_message_id(config_.smtp_host);
        const std::string body = detail::build_mime(config_, msg, message_id);
        detail::ReadCtx ctx{&body, 0};

        curl_easy_setopt(handle.get(), CURLOPT_READFUNCTION, &detail::read_cb);
        curl_easy_setopt(handle.get(), CURLOPT_READDATA, &ctx);
        curl_easy_setopt(handle.get(), CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, static_cast<long>(config_.timeout_sec));

        const CURLcode rc = curl_easy_perform(handle.get());
        curl_slist_free_all(recipients);

        if (rc != CURLE_OK) {
            spdlog::error("Mailer::send to={} failed: {}", Utils::Strings::mask_email(msg.to), curl_easy_strerror(rc));
            return false;
        }
        spdlog::info(
            "Mail sent to={} subject='{}' message_id={}", Utils::Strings::mask_email(msg.to), msg.subject, message_id);
        return true;
    }

private:
    MailerConfig config_;
};

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

inline std::unique_ptr<Mailer> global_mailer = nullptr;

inline MailerConfig load_config_from_global() {
    MailerConfig cfg;
    if (!Config::is_initialized())
        return cfg;
    auto& c = Config::get();
    cfg.enabled = c.get<bool>("mail.enabled", "MAIL_ENABLED", false);
    cfg.smtp_host = c.get<std::string>("mail.smtp_host", "MAIL_SMTP_HOST", "localhost");
    cfg.smtp_port = c.get<int>("mail.smtp_port", "MAIL_SMTP_PORT", 1025);
    cfg.smtp_username = c.get<std::string>("mail.smtp_username", "MAIL_SMTP_USERNAME", "");
    cfg.smtp_password = c.get<std::string>("mail.smtp_password", "MAIL_SMTP_PASSWORD", "");
    cfg.use_tls = c.get<bool>("mail.smtp_use_tls", "MAIL_SMTP_USE_TLS", false);
    cfg.from = c.get<std::string>("mail.from", "MAIL_FROM", "noreply@example.com");
    cfg.from_name = c.get<std::string>("mail.from_name", "MAIL_FROM_NAME", "App");
    cfg.subject_prefix = c.get<std::string>("mail.subject_prefix", "MAIL_SUBJECT_PREFIX", "[App] ");
    cfg.timeout_sec = c.get<int>("mail.timeout_sec", "MAIL_TIMEOUT_SEC", 30);
    return cfg;
}

inline void initialize() {
    if (global_mailer != nullptr)
        return;
    global_mailer = std::make_unique<Mailer>(load_config_from_global());
    spdlog::info("Mailer module initialized: enabled={} host={}:{} use_tls={}",
                 global_mailer->config().enabled,
                 global_mailer->config().smtp_host,
                 global_mailer->config().smtp_port,
                 global_mailer->config().use_tls);
}

inline bool is_initialized() {
    return global_mailer != nullptr;
}

inline Mailer& get() {
    if (!global_mailer)
        throw std::runtime_error("Mailer not initialized");
    return *global_mailer;
}

inline void shutdown() {
    global_mailer.reset();
}

}  // namespace Email
