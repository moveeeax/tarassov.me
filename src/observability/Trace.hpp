/**
 * @file Trace.hpp
 * @brief W3C Trace Context helpers — parse/format `traceparent`, generate
 *        a fresh trace ID when none is present.
 * @details Used by the HTTP pre-handling advice so every incoming request
 *          either propagates the upstream caller's trace context or gets a
 *          fresh one. The trace ID is attached to the Drogon request
 *          attributes (kTraceIdAttr) and echoed back as X-Request-Id.
 *
 *          W3C `traceparent` format:
 *            version "-" trace-id "-" parent-id "-" trace-flags
 *            00      -  32 hex    -  16 hex    -  2 hex
 *
 *          Full OTel SDK context propagation (injecting into a Span's
 *          parent context) is a natural follow-up — the skeleton here is
 *          enough for log correlation and for downstream HTTP/Kafka
 *          carriers to forward the same trace ID.
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace Observability::Trace {

inline constexpr const char* kTraceIdAttr = "_trace_id";
inline constexpr const char* kSpanIdAttr = "_span_id";
inline constexpr const char* kTraceFlagsAttr = "_trace_flags";

struct TraceContext {
    std::string trace_id;   // 32 hex chars
    std::string parent_id;  // 16 hex chars
    std::string flags;      // 2 hex chars, usually "01" (sampled) or "00"
};

namespace detail {

inline bool is_hex_lower(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

inline bool is_hex_string(std::string_view s, size_t expected_len) {
    if (s.size() != expected_len)
        return false;
    for (char c : s)
        if (!is_hex_lower(c))
            return false;
    return true;
}

inline std::string to_hex(const uint8_t* bytes, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(hex[(bytes[i] >> 4) & 0xF]);
        out.push_back(hex[bytes[i] & 0xF]);
    }
    return out;
}

inline std::mt19937_64& rng() {
    thread_local std::mt19937_64 r(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return r;
}

inline std::string random_hex(size_t byte_count) {
    std::array<uint8_t, 16> buf{};
    for (size_t i = 0; i < byte_count; i += 8) {
        uint64_t v = rng()();
        for (size_t j = 0; j < 8 && (i + j) < byte_count; ++j) {
            buf[i + j] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
    }
    return to_hex(buf.data(), byte_count);
}

inline bool is_all_zero_hex(std::string_view s) {
    for (char c : s)
        if (c != '0')
            return false;
    return !s.empty();
}

}  // namespace detail

/**
 * @brief Parse a W3C `traceparent` header value.
 * @return nullopt if the header is malformed or the ID is all zeros.
 */
inline std::optional<TraceContext> parse_traceparent(std::string_view header) {
    // Lowercase copy for case-insensitive hex comparison (the spec allows
    // lowercase only, but upstreams sometimes send mixed case).
    std::string lower(header);
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Expect "version-traceid-spanid-flags" — 4 dash-separated components.
    std::array<std::string_view, 4> parts{};
    size_t idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= lower.size(); ++i) {
        if (i == lower.size() || lower[i] == '-') {
            if (idx >= 4)
                return std::nullopt;
            parts[idx++] = std::string_view(lower).substr(start, i - start);
            start = i + 1;
        }
    }
    if (idx != 4)
        return std::nullopt;
    if (parts[0] != "00")
        return std::nullopt;  // unknown version
    if (!detail::is_hex_string(parts[1], 32))
        return std::nullopt;
    if (!detail::is_hex_string(parts[2], 16))
        return std::nullopt;
    if (!detail::is_hex_string(parts[3], 2))
        return std::nullopt;
    if (detail::is_all_zero_hex(parts[1]) || detail::is_all_zero_hex(parts[2])) {
        return std::nullopt;
    }
    TraceContext ctx;
    ctx.trace_id = std::string(parts[1]);
    ctx.parent_id = std::string(parts[2]);
    ctx.flags = std::string(parts[3]);
    return ctx;
}

/**
 * @brief Format a W3C traceparent header value.
 */
inline std::string format_traceparent(const TraceContext& ctx) {
    return "00-" + ctx.trace_id + "-" + ctx.parent_id + "-" + ctx.flags;
}

/**
 * @brief Produce a new trace context with freshly-generated IDs.
 */
inline TraceContext generate_context() {
    TraceContext ctx;
    ctx.trace_id = detail::random_hex(16);  // 16 bytes = 32 hex chars
    ctx.parent_id = detail::random_hex(8);  // 8 bytes = 16 hex chars
    ctx.flags = "01";                       // sampled by default
    return ctx;
}

/**
 * @brief Extract a trace context from a request's traceparent header,
 *        falling back to a newly-generated one if absent/malformed.
 */
inline TraceContext extract_or_generate(std::string_view traceparent_header) {
    if (!traceparent_header.empty()) {
        auto parsed = parse_traceparent(traceparent_header);
        if (parsed)
            return *parsed;
    }
    return generate_context();
}

// ---------------------------------------------------------------------------
// Ambient "current request" traceparent.
//
// Set by the HTTP tracing advice for the duration of synchronous handler
// execution (handlers run synchronously on Drogon IO threads) and read by
// Jobs::submit, so a job enqueued while serving a request carries the
// originating trace context across the process boundary to the worker. Pure
// string — no OpenTelemetry coupling — so the deliberately OTel-free Jobs
// module can read it. Empty outside a request (e.g. scheduled tasks), in which
// case the worker simply starts a fresh root span as before.
// ---------------------------------------------------------------------------
inline std::string& current_traceparent_ref() {
    thread_local std::string tp;
    return tp;
}
inline void set_current_traceparent(const TraceContext& ctx) {
    current_traceparent_ref() = format_traceparent(ctx);
}
inline void clear_current_traceparent() {
    current_traceparent_ref().clear();
}
inline std::string current_traceparent() {
    return current_traceparent_ref();
}

}  // namespace Observability::Trace
