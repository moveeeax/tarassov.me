/**
 * @file HealthController.hpp
 * @brief Health check and root endpoint controllers
 * @details Kubernetes probes (/healthz, /ready, /health) and endpoint discovery (/).
 *          /api/v1/health serves the same payload to the browser, gated: nginx
 *          proxies only /api/, so that is the one path reachable from outside the
 *          cluster, and component names plus the build version are not public
 *          information.
 */

#pragma once

#include <ctime>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "api/Endpoints.hpp"
#include "api/Guards.hpp"
#include "core/Core.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

/**
 * @brief Health check controller
 * @details Provides liveness and readiness endpoints for Kubernetes
 */
class HealthController : public HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::liveness, "/healthz", Get);
    ADD_METHOD_TO(HealthController::readiness, "/ready", Get);
    ADD_METHOD_TO(HealthController::health, "/health", Get);
    // Same payload under /api/v1, admin-gated: the SPA can only reach the
    // backend through nginx's `location /api/`, so /health alone answers
    // index.html to a fetch() that expects JSON — and unlike the in-cluster
    // probes, this path IS reachable from the internet through the ingress.
    ADD_METHOD_TO(HealthController::healthAuthed, "/api/v1/health", Get);
    METHOD_LIST_END

    void liveness(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        callback(Response::ok({{"status", "alive"}, {"timestamp", std::time(nullptr)}}));
    }

    void readiness(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        // During graceful shutdown we must report NotReady so kube-proxy
        // removes us from the Service backends before Drogon stops accepting.
        if (Core::is_shutting_down()) {
            auto resp = Response::ok({{"status", "draining"}, {"timestamp", std::time(nullptr)}});
            resp->setStatusCode(k503ServiceUnavailable);
            callback(resp);
            return;
        }
        bool ready = Core::is_initialized() && Core::health_check();
        auto resp = Response::ok({{"status", ready ? "ready" : "not_ready"}, {"timestamp", std::time(nullptr)}});
        resp->setStatusCode(ready ? k200OK : k503ServiceUnavailable);
        callback(resp);
    }

    /// The dashboard's service widget reads this. Admin-only: the widget itself
    /// requires Permission::kAdminister, and the probe's component list is not
    /// something to hand to an anonymous caller on a public origin.
    void healthAuthed(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        health(req, std::move(callback));
    }

    void health(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        // Pull every component registered via Core::register_health_check —
        // services that add their own modules no longer have to hard-code
        // lookups in this method.
        json components = json::object();
        bool critical_ok = true;         // a CRITICAL component is down → 503 unhealthy
        bool any_degraded_down = false;  // only OPTIONAL deps down → 200 degraded
        if (Core::is_initialized()) {
            for (const auto& c : Core::get().health_report()) {
                components[c.name] = {{"initialized", c.initialized}, {"healthy", c.healthy}, {"critical", c.critical}};
                if (!c.healthy) {
                    if (c.critical)
                        critical_ok = false;
                    else
                        any_degraded_down = true;
                }
            }
        } else {
            critical_ok = false;
        }
        // A degraded optional dependency (SMTP/storage/Kafka) reports "degraded"
        // but stays 200 — only a critical-component failure returns 503, matching
        // what /ready (Core::health_check) gates on.
        const char* status = !critical_ok ? "unhealthy" : (any_degraded_down ? "degraded" : "healthy");
        auto resp = Response::ok({{"status", status},
                                  {"version", Core::is_initialized() ? Core::get().version() : std::string("unknown")},
                                  {"timestamp", std::time(nullptr)},
                                  {"components", components}});
        resp->setStatusCode(critical_ok ? k200OK : k503ServiceUnavailable);
        callback(resp);
    }
};

/**
 * @brief Root controller — lists available endpoints
 */
class RootController : public HttpController<RootController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RootController::getRoot, "/", Get);
    METHOD_LIST_END

    void getRoot(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        json endpoints_json = json::array();
        for (const auto& ep : get_endpoints()) {
            endpoints_json.push_back({{"method", ep.method}, {"path", ep.path}, {"description", ep.description}});
        }
        callback(Response::ok({{"message", "C++ API Template"},
                               {"version", Core::is_initialized() ? Core::get().version() : std::string("unknown")},
                               {"endpoints", endpoints_json}}));
    }
};

}  // namespace Api
