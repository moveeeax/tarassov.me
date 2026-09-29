/**
 * @file DashboardWidgetRepository.hpp
 * @brief Owner-scoped access to dashboard_widgets (migration 010). CrudBase
 *        supplies the reads; the one hand-written write replaces a whole layout,
 *        because the grid always hands over a complete arrangement.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "database/Database.hpp"
#include "domain/DashboardWidget.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Repositories {

/// → 404. The owner_id foreign key failed, which means the user row is gone: an
/// access token can outlive the account it names, and the INSERT is where that
/// shows up. Reported as the missing user rather than a bare 500.
struct DashboardOwnerGone : NotFoundError {
    DashboardOwnerGone() : NotFoundError("user") {}
};

/// → 409. A future unique index or CHECK on dashboard_widgets lands here instead
/// of falling through to the 500 arm of with_repo_errors.
struct DashboardWidgetConflict : ConflictError {
    DashboardWidgetConflict() : ConflictError("dashboard_widget_conflict", "layout violates a database constraint") {}
};

/// Validated input row the controller hands to replace_all. `options_json` is
/// already-serialized JSON (the controller dumps the validated object), so the
/// repository never needs to know the option rules.
struct DashboardWidgetInput {
    std::string widget_type;
    int grid_x{0};
    int grid_y{0};
    int grid_w{0};
    int grid_h{0};
    std::string options_json{"{}"};
};

class DashboardWidgetRepository : public CrudBase<DashboardWidgetRepository, Domain::DashboardWidget, std::string> {
public:
    static constexpr const char* kTable = "dashboard_widgets";
    // options::text so libpqxx hands the DTO a string it can parse — reading
    // jsonb as std::string directly is driver-dependent.
    static constexpr const char* kColumns =
        "id, owner_id, widget_type, grid_x, grid_y, grid_w, grid_h, options::text AS options, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "grid_y, grid_x";
    // Unlocks find_owned / list_owned / count_owned. Plain find/list on this
    // table would be an IDOR — never call them.
    static constexpr const char* kOwnerColumn = "owner_id";

    /**
     * @brief Replace the owner's entire layout in one transaction.
     * @details Drag-and-drop produces a whole new arrangement, so there is no
     *          per-widget patch: delete-then-insert inside a single write keeps
     *          the layout from ever being half-applied. Returns the stored rows
     *          (fresh ids) in insertion order.
     */
    std::vector<Domain::DashboardWidget> replace_all(const std::string& owner_id,
                                                     const std::vector<DashboardWidgetInput>& widgets) {
        return detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    txn.exec_params("DELETE FROM dashboard_widgets WHERE owner_id = $1", owner_id);
                    std::vector<Domain::DashboardWidget> out;
                    out.reserve(widgets.size());
                    for (const auto& w : widgets) {
                        auto r = txn.exec_params(
                            std::string("INSERT INTO dashboard_widgets "
                                        "(owner_id, widget_type, grid_x, grid_y, grid_w, grid_h, options) "
                                        "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb) RETURNING ") +
                                kColumns,
                            owner_id,
                            w.widget_type,
                            w.grid_x,
                            w.grid_y,
                            w.grid_w,
                            w.grid_h,
                            w.options_json);
                        out.push_back(Domain::DashboardWidget::from_row(r[0]));
                    }
                    return out;
                });
            },
            [](std::string_view state) {
                if (state == "23503")  // foreign_key_violation: the owner is gone
                    throw DashboardOwnerGone{};
                if (state == "23505" || state == "23514")  // unique_violation / check_violation
                    throw DashboardWidgetConflict{};
            });
    }
};

}  // namespace Repositories
