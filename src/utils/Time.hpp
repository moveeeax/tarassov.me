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

}  // namespace Utils::Time
