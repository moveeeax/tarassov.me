/**
 * @file test_page_templates.cpp
 * @brief Unit tests for the SSR page-template loader/renderer
 *        (src/pages/PageTemplates.hpp): substitution semantics, the
 *        no-re-expansion guarantee, unknown placeholders, missing templates.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "pages/PageTemplates.hpp"
#include "test_helpers.hpp"
#include "utils/Config.hpp"

namespace {

class PageTemplatesTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string config_path_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "page-templates-test";
        std::filesystem::create_directories(dir_);
        setenv("SITE_PAGES_TEMPLATES_DIR", dir_.string().c_str(), 1);
        config_path_ = TestHelpers::create_temp_config("{}", "page_templates_test_config.json");
        Config::initialize(config_path_);
    }

    void TearDown() override {
        Config::shutdown();
        unsetenv("SITE_PAGES_TEMPLATES_DIR");
        std::filesystem::remove_all(dir_);
    }

    // load() caches by name for the process lifetime — every test writes a
    // UNIQUE template name so cached content can't leak across tests.
    void write(const std::string& name, const std::string& content) {
        std::ofstream(dir_ / (name + ".html")) << content;
    }
};

TEST_F(PageTemplatesTest, SubstitutesAllOccurrences) {
    write("pt_basic", "<t>{{TITLE}}</t><d>{{DESC}}</d><t2>{{TITLE}}</t2>");
    const auto out = Pages::render("pt_basic", {{"TITLE", "Hello"}, {"DESC", "World"}});
    EXPECT_EQ(out, "<t>Hello</t><d>World</d><t2>Hello</t2>");
}

TEST_F(PageTemplatesTest, ValuesAreNeverReExpanded) {
    // A post whose CONTENT contains a literal "{{DESC}}" must render it
    // literally — the old multi-pass renderer expanded placeholders inside
    // previously substituted values.
    write("pt_noreexpand", "<body>{{BODY}}|{{DESC}}</body>");
    const auto out = Pages::render("pt_noreexpand", {{"BODY", "look: {{DESC}} stays"}, {"DESC", "real"}});
    EXPECT_EQ(out, "<body>look: {{DESC}} stays|real</body>");
}

TEST_F(PageTemplatesTest, UnknownPlaceholderIsLeftAsIs) {
    write("pt_unknown", "a {{NOPE}} b");
    EXPECT_EQ(Pages::render("pt_unknown", {}), "a {{NOPE}} b");
}

TEST_F(PageTemplatesTest, UnterminatedPlaceholderCopiedVerbatim) {
    write("pt_unterminated", "x {{BROKEN and the rest");
    EXPECT_EQ(Pages::render("pt_unterminated", {{"BROKEN", "nope"}}), "x {{BROKEN and the rest");
}

TEST_F(PageTemplatesTest, EmptyValueErasesPlaceholder) {
    write("pt_empty", "[{{ROBOTS}}]");
    EXPECT_EQ(Pages::render("pt_empty", {{"ROBOTS", ""}}), "[]");
}

TEST_F(PageTemplatesTest, MissingTemplateThrows) {
    EXPECT_THROW(Pages::render("pt_does_not_exist", {}), std::runtime_error);
}

}  // namespace
