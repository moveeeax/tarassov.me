/**
 * @file AuditController.hpp
 * @brief Read-only admin view of the audit trail.
 *
 * Route:
 *   GET /api/admin/audit   list audit_log entries (paginated, newest first)
 *
 * Gated by the dedicated Permission::kAuditRead bit (not full-admin) so a
 * read-only "auditor" role is possible — full admins hold every 0xff bit and
 * pass automatically. Filters: ?action= &actor_id= &target_type= &from= &to=.
 * from/to must be RFC 3339 date-times — anything else is a 400, not a 500 from
 * the timestamptz cast.
 */

#pragma once

#include <optional>
#include <regex>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "domain/AuditEntry.hpp"
#include "domain/Role.hpp"
#include "repositories/AuditRepository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AuditController : public HttpController<AuditController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuditController::listAudit, "/api/v1/admin/audit", Get);
    METHOD_LIST_END

    void listAudit(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_PERMISSION(req, callback, Domain::Permission::kAuditRead);

        const auto pp = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        auto param = [&](const char* key) -> std::optional<std::string> {
            auto v = req->getParameter(key);
            return v.empty() ? std::nullopt : std::optional<std::string>{v};
        };
        Repositories::AuditRepository::Filters f;
        f.action = param("action");
        f.actor_id = param("actor_id");
        f.target_type = param("target_type");
        f.from = param("from");
        f.to = param("to");

        // ?from= / ?to= are bound as $N::timestamptz. An unparsable value raises
        // SQLSTATE 22007/22008, which AuditRepository doesn't run through
        // translate_sql — so it used to surface as a 500 (plus a doubled query
        // and a misleading replica-failover warning) against a spec that
        // declares format: date-time. Validate here and return the 400 envelope.
        // The datetime-local form inputs emit "YYYY-MM-DDTHH:MM", which Postgres
        // accepts, so the zone and seconds are optional.
        static const std::regex kRfc3339(
            R"(^\d{4}-\d{2}-\d{2}[Tt ]\d{2}:\d{2}(:\d{2}(\.\d+)?)?([Zz]|[+-]\d{2}:?\d{2})?$)");
        Validation::Errors date_errs;
        json dates = json::object();
        if (f.from)
            dates["from"] = *f.from;
        if (f.to)
            dates["to"] = *f.to;
        Validation::regex_match(date_errs, dates, "from", kRfc3339, "RFC 3339 date-time");
        Validation::regex_match(date_errs, dates, "to", kRfc3339, "RFC 3339 date-time");
        if (date_errs.any()) {
            callback(Validation::response_400(date_errs));
            return;
        }

        with_repo_errors(callback, "admin listAudit", [&] {
            Repositories::AuditRepository repo;
            auto page = repo.list_filtered(f, pp.limit, pp.offset);
            json data = json::array();
            for (const auto& entry : page.entries)
                data.push_back(entry);
            callback(Response::paginated(data, page.total, pp.limit, pp.offset));
        });
    }
};

}  // namespace Api
