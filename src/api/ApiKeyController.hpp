/**
 * @file ApiKeyController.hpp
 * @brief Manage the caller's own API keys: create / list / revoke. Owner-scoped
 *        (API_REQUIRE_OWNER) so a user only ever sees or revokes their own keys.
 *        The secret is returned exactly ONCE, from create().
 */

#pragma once

#include <functional>
#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/Validation.hpp"
#include "repositories/ApiKeyRepository.hpp"
#include "security/ApiKeys.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class ApiKeyController : public HttpController<ApiKeyController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ApiKeyController::list, "/api/v1/account/api-keys", Get);
    ADD_METHOD_TO(ApiKeyController::create, "/api/v1/account/api-keys", Post);
    ADD_METHOD_TO(ApiKeyController::remove, "/api/v1/account/api-keys/{1}", Delete);
    METHOD_LIST_END

    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);
        Repositories::ApiKeyRepository repo;
        auto keys = repo.list_for_user(owner);
        json data = json::array();
        for (const auto& k : keys)
            data.push_back(k);
        callback(Response::ok({{"data", data}, {"total", data.size()}}));
    }

    void create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_OWNER(req, callback, owner);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "name");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const auto gen = Security::ApiKeys::generate();
        Repositories::ApiKeyRepository repo;
        auto key = repo.create(owner, body["name"].get<std::string>(), gen.key_hash, gen.prefix);

        // The plaintext key is surfaced ONCE here; it is never stored (only its
        // hash) and can never be shown again. The client must save it now.
        json out = key;
        out["key"] = gen.plaintext;
        callback(Response::created(out));
    }

    void remove(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback,
                const std::string& id) {
        API_REQUIRE_OWNER(req, callback, owner);
        Repositories::ApiKeyRepository repo;
        if (!repo.revoke(id, owner)) {
            // Not found, already revoked, or someone else's key — all the same
            // 404 so a caller can't probe which key ids exist.
            callback(ErrorResponse::not_found("api_key"));
            return;
        }
        callback(Response::ok({{"message", "API key revoked"}}));
    }
};

}  // namespace Api
