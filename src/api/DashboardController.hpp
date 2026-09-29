/**
 * @file DashboardController.hpp
 * @brief The caller's own dashboard: the widget catalog and the saved layout.
 *        Owner-scoped (API_REQUIRE_OWNER) so one user never reads or writes
 *        another's arrangement. PUT replaces the whole layout: dragging a card
 *        produces a new arrangement, not a patch, and a full replace is
 *        idempotent for free.
 *
 * Deliberately NOT validated: overlaps between widgets. react-grid-layout never
 * produces them, and rejecting them here would add a 400 with no reader behind it.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <drogon/HttpController.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "domain/WidgetCatalog.hpp"
#include "repositories/DashboardWidgetRepository.hpp"
#include "security/Auth.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class DashboardController : public HttpController<DashboardController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DashboardController::catalog, "/api/v1/dashboard/catalog", Get);
    ADD_METHOD_TO(DashboardController::layout, "/api/v1/dashboard/layout", Get);
    ADD_METHOD_TO(DashboardController::saveLayout, "/api/v1/dashboard/layout", Put);
    METHOD_LIST_END

    /// The widget types this caller may place. Filtering here (rather than only
    /// on PUT) is what keeps a widget the caller can't use out of the picker.
    void catalog(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);
        (void)owner;  // identity is required; the catalog itself is per-permission
        callback(Response::list(Domain::Widgets::catalog_json(
            // require_permission returns nullptr when the caller holds the bit
            // AND when auth is disabled — reusing it keeps this gate identical
            // to every other one instead of re-deriving the dev-mode escape.
            [&](std::uint32_t perm) { return !Security::Auth::require_permission(req, perm); })));
    }

    void layout(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);
        Repositories::DashboardWidgetRepository repo;
        with_repo_errors(callback, "dashboard layout", [&] {
            // from_primary: the client refetches the layout straight after a PUT,
            // and a replica read can answer with the arrangement it just replaced.
            auto widgets = repo.list_owned(owner, static_cast<int>(Domain::Widgets::kMaxWidgets), 0, true);
            json data = json::array();
            for (const auto& w : widgets)
                data.push_back(w);
            callback(Response::list(data));
        });
    }

    void saveLayout(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);

        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        if (!body.contains("widgets") || !body["widgets"].is_array()) {
            errs.add("widgets", "not_array", "must be an array");
            callback(Validation::response_400(errs));
            return;
        }
        const auto& items = body["widgets"];
        if (items.size() > Domain::Widgets::kMaxWidgets) {
            errs.add("widgets", "too_many", "max " + std::to_string(Domain::Widgets::kMaxWidgets));
            callback(Validation::response_400(errs));
            return;
        }

        std::vector<Repositories::DashboardWidgetInput> inputs;
        inputs.reserve(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& w = items[i];
            const std::string field = "widgets[" + std::to_string(i) + "]";
            if (!w.is_object()) {
                errs.add(field, "not_object", "must be an object");
                continue;
            }
            if (!w.contains("widget_type") || !w["widget_type"].is_string()) {
                errs.add(field + ".widget_type", "invalid", "must be a string");
                continue;
            }
            const auto type = w["widget_type"].get<std::string>();
            const auto* spec = Domain::Widgets::find(type);
            if (!spec) {
                errs.add(field + ".widget_type", "unknown_widget", "not in the catalog");
                continue;
            }
            // A widget the caller may not read is an access question, not a
            // shape question — 403, and the whole request stops here.
            if (auto forbidden = Security::Auth::require_permission(req, spec->required_permission)) {
                callback(forbidden);
                return;
            }

            const std::size_t before = errs.items().size();
            Domain::Widgets::validate_widget(errs, w, *spec, i);
            if (errs.items().size() != before)
                continue;  // keep collecting errors from later widgets, skip this one

            Repositories::DashboardWidgetInput in;
            in.widget_type = type;
            in.grid_x = w["grid_x"].get<int>();
            in.grid_y = w["grid_y"].get<int>();
            in.grid_w = w["grid_w"].get<int>();
            in.grid_h = w["grid_h"].get<int>();
            in.options_json = (w.contains("options") && w["options"].is_object()) ? w["options"].dump() : "{}";
            inputs.push_back(std::move(in));
        }

        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        Repositories::DashboardWidgetRepository repo;
        with_repo_errors(callback, "dashboard saveLayout", [&] {
            auto stored = repo.replace_all(owner, inputs);
            json data = json::array();
            for (const auto& w : stored)
                data.push_back(w);
            callback(Response::list(data));
        });
    }
};

}  // namespace Api
