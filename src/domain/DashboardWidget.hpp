/**
 * @file DashboardWidget.hpp
 * @brief One widget instance on a user's dashboard (dashboard_widgets table,
 *        migration 010). Type, position and size are columns; `options` carries
 *        the widget's own parameters and is validated against the catalog
 *        (domain/WidgetCatalog.hpp) before it ever reaches this DTO.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "utils/Time.hpp"

namespace Domain {

struct DashboardWidget {
    std::string id;
    std::string owner_id;
    std::string widget_type;
    int grid_x{0};
    int grid_y{0};
    int grid_w{0};
    int grid_h{0};
    nlohmann::json options = nlohmann::json::object();
    // Timestamps are ISO 8601 — normalized in from_row(), as Post and ApiKey do.
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static DashboardWidget from_row(const Row& row) {
        DashboardWidget w;
        w.id = row["id"].template as<std::string>();
        w.owner_id = row["owner_id"].template as<std::string>();
        w.widget_type = row["widget_type"].template as<std::string>();
        w.grid_x = row["grid_x"].template as<int>();
        w.grid_y = row["grid_y"].template as<int>();
        w.grid_w = row["grid_w"].template as<int>();
        w.grid_h = row["grid_h"].template as<int>();
        // jsonb comes back as text (kColumns casts it explicitly). Parse at this
        // single DB→domain boundary so no handler ever sees a string where the
        // contract promises an object.
        w.options = nlohmann::json::parse(row["options"].template as<std::string>());
        w.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
        w.updated_at = Utils::Time::pg_to_iso8601(row["updated_at"].template as<std::string>());
        return w;
    }
};

inline void to_json(nlohmann::json& j, const DashboardWidget& w) {
    // owner_id is intentionally absent: the caller IS the owner on every route
    // that serves this DTO, so echoing a user id would add nothing but exposure.
    j = nlohmann::json{{"id", w.id},
                       {"widget_type", w.widget_type},
                       {"grid_x", w.grid_x},
                       {"grid_y", w.grid_y},
                       {"grid_w", w.grid_w},
                       {"grid_h", w.grid_h},
                       {"options", w.options},
                       {"created_at", w.created_at},
                       {"updated_at", w.updated_at}};
}

}  // namespace Domain
