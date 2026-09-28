/**
 * @file WidgetCatalog.hpp
 * @brief The dashboard widget catalog: the single list of widget types the
 *        backend accepts and the frontend renders. GET /api/v1/dashboard/catalog
 *        publishes it; PUT /api/v1/dashboard/layout validates against it. Keeping
 *        both on one constexpr array is what stops a second list from drifting.
 *
 * Pure logic: no HTTP, no SQL. The permission FILTER is injected by the caller
 * (the controller passes Security::Auth::require_permission), so this header
 * stays testable without booting Core.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "api/Validation.hpp"
#include "domain/Role.hpp"

namespace Domain::Widgets {

using json = nlohmann::json;

/// The frontend renders a 12-column grid; these bounds are what the API enforces.
inline constexpr int kGridColumns = 12;
inline constexpr int kMaxGridY = 200;
inline constexpr int kMaxGridH = 24;
inline constexpr std::size_t kMaxWidgets = 40;

/// One integer option a widget accepts. Widgets have no string options yet —
/// adding one means a new spec kind here, never a free-form passthrough.
struct OptionSpec {
    std::string_view key;
    long long min_value;
    long long max_value;
    long long default_value;
};

struct WidgetSpec {
    std::string_view type;
    std::string_view title;
    std::string_view description;
    /// Permission bit the caller must hold. kAdminister for admin-only widgets.
    std::uint32_t required_permission;
    int default_w;
    int default_h;
    int min_w;
    int min_h;
    const OptionSpec* options;
    std::size_t option_count;
};

inline constexpr std::array<OptionSpec, 1> kPostsOptions{{{"limit", 1, 10, 5}}};
inline constexpr std::array<OptionSpec, 1> kJobsOptions{{{"window_days", 1, 30, 7}}};
inline constexpr std::array<OptionSpec, 1> kAuditOptions{{{"limit", 1, 20, 10}}};
inline constexpr std::array<OptionSpec, 1> kUsersOptions{{{"limit", 1, 10, 5}}};
inline constexpr std::array<OptionSpec, 1> kNoOptions{{{"", 0, 0, 0}}};

/// Order here is the order a fresh dashboard offers them in.
inline constexpr std::array<WidgetSpec, 5> kCatalog{{
    {"posts_summary",
     "Posts",
     "Drafts, published and the latest entries",
     Permission::kAdminister,
     4,
     3,
     3,
     2,
     kPostsOptions.data(),
     kPostsOptions.size()},
    {"jobs_queue",
     "Jobs",
     "Queue by status and DLQ size",
     Permission::kAdminister,
     4,
     3,
     3,
     2,
     kJobsOptions.data(),
     kJobsOptions.size()},
    {"audit_recent",
     "Audit",
     "Latest admin actions",
     Permission::kAuditRead,
     6,
     4,
     4,
     3,
     kAuditOptions.data(),
     kAuditOptions.size()},
    {"users_recent",
     "Users",
     "User count and latest registrations",
     Permission::kAdminister,
     4,
     3,
     3,
     2,
     kUsersOptions.data(),
     kUsersOptions.size()},
    // service_health takes no options: kNoOptions carries a single empty-key
    // entry (a zero-length std::array is legal but data() may be nullptr), and
    // option_count = 0 keeps it invisible to both the JSON and the validator.
    {"service_health", "Service", "Readiness and version", Permission::kAdminister, 4, 2, 2, 2, kNoOptions.data(), 0},
}};

inline const WidgetSpec* find(std::string_view type) {
    for (const auto& spec : kCatalog)
        if (spec.type == type)
            return &spec;
    return nullptr;
}

/**
 * @brief Serialize the catalog, keeping only specs the caller may place.
 * @param permitted Predicate over a permission bit: true when the caller holds
 *        it. The controller passes a lambda over
 *        Security::Auth::require_permission so the auth-disabled escape hatch
 *        behaves exactly as it does for every other guard.
 */
template <typename PermitFn>
inline json catalog_json(PermitFn&& permitted) {
    json out = json::array();
    for (const auto& spec : kCatalog) {
        if (!permitted(spec.required_permission))
            continue;
        json options = json::object();
        for (std::size_t i = 0; i < spec.option_count; ++i) {
            const auto& opt = spec.options[i];
            options[std::string(opt.key)] = {
                {"type", "int"}, {"min", opt.min_value}, {"max", opt.max_value}, {"default", opt.default_value}};
        }
        out.push_back({{"type", std::string(spec.type)},
                       {"title", std::string(spec.title)},
                       {"description", std::string(spec.description)},
                       {"default_w", spec.default_w},
                       {"default_h", spec.default_h},
                       {"min_w", spec.min_w},
                       {"min_h", spec.min_h},
                       {"options", options}});
    }
    return out;
}

namespace detail {

/// Read an integer field within [min_v, max_v], reporting under `reported`
/// (which carries the widget index) instead of the bare key. Hand-rolled rather
/// than Validation::int_range because that one reports the bare field name, and
/// a 400 on a 12-widget layout must say WHICH widget is wrong.
inline bool int_field(Api::Validation::Errors& errs,
                      const json& obj,
                      const std::string& key,
                      const std::string& reported,
                      long long min_v,
                      long long max_v,
                      long long& out) {
    if (!obj.contains(key) || obj[key].is_null()) {
        errs.add(reported, "missing", "required");
        return false;
    }
    // is_number_integer() is false for 1.5 — without this check the get<>
    // below throws nlohmann's type_error.302, which escapes as a bare 500.
    if (!obj[key].is_number_integer()) {
        errs.add(reported, "not_integer", "must be an integer");
        return false;
    }
    out = obj[key].get<long long>();
    if (out < min_v) {
        errs.add(reported, "below_min", "min " + std::to_string(min_v));
        return false;
    }
    if (out > max_v) {
        errs.add(reported, "above_max", "max " + std::to_string(max_v));
        return false;
    }
    return true;
}

}  // namespace detail

/**
 * @brief Validate one widget's geometry and options against its spec.
 * @param index Position in the request's `widgets` array; every error field is
 *        prefixed `widgets[<index>].` so the client can point at the entry.
 */
inline void validate_widget(Api::Validation::Errors& errs,
                            const json& widget,
                            const WidgetSpec& spec,
                            std::size_t index) {
    const std::string prefix = "widgets[" + std::to_string(index) + "].";
    long long x = 0;
    long long width = 0;
    long long scratch = 0;

    const bool have_x = detail::int_field(errs, widget, "grid_x", prefix + "grid_x", 0, kGridColumns - 1, x);
    detail::int_field(errs, widget, "grid_y", prefix + "grid_y", 0, kMaxGridY, scratch);
    const bool have_w = detail::int_field(errs, widget, "grid_w", prefix + "grid_w", spec.min_w, kGridColumns, width);
    detail::int_field(errs, widget, "grid_h", prefix + "grid_h", spec.min_h, kMaxGridH, scratch);

    if (have_x && have_w && x + width > kGridColumns) {
        errs.add(prefix + "grid_w", "out_of_grid", "grid_x + grid_w must not exceed " + std::to_string(kGridColumns));
    }

    // options is optional; absent means "spec defaults".
    if (!widget.contains("options") || widget["options"].is_null())
        return;
    if (!widget["options"].is_object()) {
        errs.add(prefix + "options", "not_object", "must be an object");
        return;
    }
    for (auto it = widget["options"].begin(); it != widget["options"].end(); ++it) {
        const OptionSpec* opt = nullptr;
        for (std::size_t i = 0; i < spec.option_count; ++i) {
            if (spec.options[i].key == it.key()) {
                opt = &spec.options[i];
                break;
            }
        }
        if (!opt) {
            // Silently dropping an unknown key would let a stale client think a
            // setting took effect. Fail loudly instead.
            errs.add(prefix + "options." + it.key(), "unknown_option", "not accepted by " + std::string(spec.type));
            continue;
        }
        detail::int_field(errs,
                          widget["options"],
                          std::string(opt->key),
                          prefix + "options." + it.key(),
                          opt->min_value,
                          opt->max_value,
                          scratch);
    }
}

}  // namespace Domain::Widgets
