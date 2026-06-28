#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/Api.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

// ---------- RootController Tests ----------

class RootEndpointTest : public TestHelpers::CoreBackedTest {
protected:
    Api::RootController controller;

    std::string config_file_name() const override { return "root_test_config.json"; }
};

TEST_F(RootEndpointTest, GetRoot) {
    auto req = TestHelpers::make_request();
    HttpResponsePtr captured;

    controller.getRoot(req, [&](const HttpResponsePtr& resp) { captured = resp; });

    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->statusCode(), k200OK);

    auto body = json::parse(std::string(captured->body()));
    EXPECT_EQ(body["message"], "C++ API Template");
    EXPECT_TRUE(body.contains("version"));
    EXPECT_TRUE(body.contains("endpoints"));
    EXPECT_TRUE(body["endpoints"].is_array());
    // The discovery endpoint must reflect the registry exactly — comparing
    // against the single source of truth instead of a magic count.
    EXPECT_EQ(body["endpoints"].size(), Api::get_endpoints().size());
}
