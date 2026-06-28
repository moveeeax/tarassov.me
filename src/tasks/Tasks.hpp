/**
 * @file Tasks.hpp
 * @brief Recurring background tasks scheduled on Drogon's event loop.
 *
 * Single user today: Core::register_dlq_metric_ schedules a periodic
 * Redis scan to refresh the jobs DLQ Prometheus gauge. Kept as a thin
 * facade so callers don't reach into Drogon directly and so cancel_all()
 * can drain timers cleanly on Core::shutdown().
 */

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

namespace Tasks {

using TaskCallback = std::function<void()>;

namespace detail {

inline std::map<std::string, trantor::TimerId>& timers() {
    static std::map<std::string, trantor::TimerId> m;
    return m;
}

inline std::mutex& mu() {
    static std::mutex m;
    return m;
}

inline bool& initialized_flag() {
    static bool b = false;
    return b;
}

}  // namespace detail

inline void initialize() {
    if (detail::initialized_flag())
        throw std::runtime_error("Tasks already initialized");
    detail::initialized_flag() = true;
    spdlog::info("Task scheduler initialized");
}

inline bool is_initialized() {
    return detail::initialized_flag();
}

inline bool schedule_recurring(const std::string& task_id, std::chrono::milliseconds interval, TaskCallback callback) {
    if (!detail::initialized_flag())
        throw std::runtime_error("Tasks not initialized");
    std::lock_guard<std::mutex> lock(detail::mu());
    if (detail::timers().count(task_id) > 0) {
        spdlog::warn("Task '{}' already scheduled", task_id);
        return false;
    }
    auto id = drogon::app().getLoop()->runEvery(interval.count() / 1000.0, [task_id, callback]() {
        try {
            callback();
        } catch (const std::exception& e) {
            spdlog::error("Recurring task '{}' failed: {}", task_id, e.what());
        }
    });
    detail::timers()[task_id] = id;
    return true;
}

inline bool cancel(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(detail::mu());
    auto it = detail::timers().find(task_id);
    if (it == detail::timers().end())
        return false;
    drogon::app().getLoop()->invalidateTimer(it->second);
    detail::timers().erase(it);
    return true;
}

inline void shutdown() {
    if (!detail::initialized_flag())
        return;
    std::lock_guard<std::mutex> lock(detail::mu());
    for (auto& [id, tid] : detail::timers()) {
        drogon::app().getLoop()->invalidateTimer(tid);
    }
    detail::timers().clear();
    detail::initialized_flag() = false;
    spdlog::info("Task scheduler shut down");
}

}  // namespace Tasks
