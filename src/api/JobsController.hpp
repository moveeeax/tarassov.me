/**
 * @file JobsController.hpp
 * @brief Jobs queue HTTP controller
 * @details Handles /api/jobs endpoints for submitting and querying background jobs
 */

#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include "api/Guards.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "jobs/Jobs.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;

// The jobs API is an ops surface (queue state, payloads, requeue/cancel) —
// admin-only when auth is on (API_REQUIRE_ADMIN no-ops under AUTH_MODE=none).

/**
 * @brief Jobs controller — submit, query, and cancel background jobs
 */
class JobsController : public HttpController<JobsController> {
public:
    METHOD_LIST_BEGIN
    // Fixed-path routes must be registered before parameterized routes so
    // that /api/jobs/dlq does not accidentally match /api/jobs/{1}.
    ADD_METHOD_TO(JobsController::listDlq, "/api/v1/jobs/dlq", Get);
    ADD_METHOD_TO(JobsController::requeueDlq, "/api/v1/jobs/dlq/{1}/requeue", Post);
    ADD_METHOD_TO(JobsController::listJobs, "/api/v1/jobs", Get);
    ADD_METHOD_TO(JobsController::submitJob, "/api/v1/jobs", Post);
    ADD_METHOD_TO(JobsController::getJobStatus, "/api/v1/jobs/{1}", Get);
    ADD_METHOD_TO(JobsController::cancelJob, "/api/v1/jobs/{1}", Delete);
    METHOD_LIST_END

    void listJobs(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        API_REQUIRE_JOBS_READY(callback);
        try {
            auto type_param = req->getParameter("type");
            const auto pp = parse_page_params(req, /*default_limit=*/20, /*max_limit=*/200);

            auto page = Jobs::get().list_paged(type_param, pp.limit, pp.offset);
            json jobs_json = json::array();
            for (const auto& job : page.jobs) {
                jobs_json.push_back(job.to_json());
            }
            callback(Response::paginated(jobs_json, page.total, pp.limit, pp.offset));
        } catch (const std::exception& e) {
            spdlog::error("Error in GET /api/jobs: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    void submitJob(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        API_REQUIRE_JOBS_READY(callback);
        try {
            json body;
            if (!Validation::parse_body(req, body, callback))
                return;

            Validation::Errors errs;
            Validation::require(errs, body, "type");
            Validation::string_length(errs, body, "type", 1, 255);
            if (errs.any()) {
                callback(Validation::response_400(errs));
                return;
            }

            auto type = body["type"].get<std::string>();
            auto payload = body.value("payload", json::object());
            int max_retries = body.value("max_retries", -1);

            auto job = Jobs::get().submit(type, payload, max_retries);
            callback(Response::created({{"data", job.to_json()}, {"message", "Job submitted"}}));
        } catch (const std::exception& e) {
            spdlog::error("Error in POST /api/jobs: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    void getJobStatus(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        API_REQUIRE_JOBS_READY(callback);
        try {
            if (!is_valid_uuid(id)) {
                callback(ErrorResponse::bad_request("invalid_uuid", "UUID format is invalid"));
                return;
            }
            auto job = Jobs::get().get_status(id);
            if (!job) {
                callback(ErrorResponse::not_found("job"));
                return;
            }
            callback(Response::ok({{"data", job->to_json()}}));
        } catch (const std::exception& e) {
            spdlog::error("Error in GET /api/jobs/{}: {}", id, e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    void listDlq(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        API_REQUIRE_JOBS_READY(callback);
        try {
            auto type_param = req->getParameter("type");
            int limit = clamp_int(req->getParameter("limit"), 100, 1, 500);

            auto jobs = Jobs::get().list_dlq(type_param, limit);
            json jobs_json = json::array();
            for (const auto& j : jobs)
                jobs_json.push_back(j.to_json());
            callback(
                Response::ok({{"data", jobs_json}, {"count", jobs_json.size()}, {"depth", Jobs::get().dlq_depth()}}));
        } catch (const std::exception& e) {
            spdlog::error("Error in GET /api/jobs/dlq: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    void requeueDlq(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        API_REQUIRE_JOBS_READY(callback);
        try {
            if (!is_valid_uuid(id)) {
                callback(ErrorResponse::bad_request("invalid_uuid", "UUID format is invalid"));
                return;
            }
            if (!Jobs::get().requeue_from_dlq(id)) {
                callback(ErrorResponse::not_found("dlq_job"));
                return;
            }
            callback(Response::ok({{"message", "Job requeued from DLQ"}, {"id", id}}));
        } catch (const std::exception& e) {
            spdlog::error("Error in POST /api/jobs/dlq/{}/requeue: {}", id, e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    void cancelJob(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& id) {
        API_REQUIRE_ADMIN(req, callback);
        API_REQUIRE_JOBS_READY(callback);
        try {
            if (!is_valid_uuid(id)) {
                callback(ErrorResponse::bad_request("invalid_uuid", "UUID format is invalid"));
                return;
            }
            if (!Jobs::get().cancel(id)) {
                callback(ErrorResponse::not_found("cancellable_job"));
                return;
            }
            callback(Response::ok({{"message", "Job cancelled"}}));
        } catch (const std::exception& e) {
            spdlog::error("Error in DELETE /api/jobs/{}: {}", id, e.what());
            callback(ErrorResponse::internal_error());
        }
    }
};

}  // namespace Api
