/**
 * @file Time.hpp
 * @brief Epoch-time helpers shared across modules.
 *
 * Centralizes the `duration_cast<...>(system_clock::now().time_since_epoch())`
 * idiom that was previously open-coded in Auth, Jobs, Tokens, RateLimit and
 * the auth controllers. One source of truth for "what time is it (epoch)".
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace Utils::Time {

/// Seconds since the Unix epoch.
inline std::int64_t now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Milliseconds since the Unix epoch.
inline std::int64_t now_epoch_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// "2026-07-25 05:34:32.87643+00[:00]" (libpqxx text form) → ISO 8601.
/// Space→'T', fractional seconds dropped, "+00"/"+00:00"→"Z"; other offsets
/// kept (still valid ISO 8601). Inputs that don't look like
/// "YYYY-MM-DD HH:MM:SS..." are returned unchanged.
inline std::string pg_to_iso8601(const std::string& ts) {
    if (ts.size() < 19 || ts[10] != ' ')
        return ts;
    std::string out = ts.substr(0, 10) + "T" + ts.substr(11, 8);
    std::size_t i = 19;
    if (i < ts.size() && ts[i] == '.') {  // fractional seconds
        ++i;
        while (i < ts.size() && ts[i] >= '0' && ts[i] <= '9')
            ++i;
    }
    const std::string offset = ts.substr(i);
    if (offset == "+00" || offset == "+00:00" || offset == "Z" || offset.empty())
        out += "Z";
    else
        out += offset;
    return out;
}

/// Unix seconds → "YYYY-MM-DDTHH:MM:SSZ" (UTC).
inline std::string epoch_to_iso8601(std::int64_t secs) {
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace Utils::Time
