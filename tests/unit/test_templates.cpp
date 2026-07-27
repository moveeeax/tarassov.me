/**
 * @file test_templates.cpp
 * @brief Unit tests for the inja email-template renderer.
 *
 * Pure unit bucket: renders against a temp directory created per test —
 * no Config, no network, no sidecars. The repo's real templates under
 * templates/email are exercised too (substitution smoke), so a broken
 * placeholder fails here instead of as a swallowed warn at send time.
 */

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "email/Templates.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class TemplatesTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<TestHelpers::ScopedEnv> dir_env_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "tpl_test";
        fs::create_directories(dir_);
        dir_env_ = std::make_unique<TestHelpers::ScopedEnv>("MAIL_TEMPLATES_DIR", dir_.string());
    }

    void TearDown() override {
        dir_env_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void write(const std::string& file, const std::string& content) { std::ofstream(dir_ / file) << content; }
};

TEST_F(TemplatesTest, renderSubstitutesContext) {
    write("greet.txt", "Hello {{ user.name }}, welcome to {{ app_name }}!");
    auto out = Email::Templates::render("greet", "txt", {{"user", {{"name", "Ada"}}}, {"app_name", "App"}});
    EXPECT_EQ(out, "Hello Ada, welcome to App!");
}

TEST_F(TemplatesTest, renderPairReturnsBothVariants) {
    write("note.txt", "text: {{ v }}");
    write("note.html", "<b>{{ v }}</b>");
    auto pair = Email::Templates::render_pair("note", {{"v", "x"}});
    EXPECT_EQ(pair.text, "text: x");
    EXPECT_EQ(pair.html, "<b>x</b>");
}

// ── H2: the .html path escapes context values, the .txt path does not ───────
// inja has NO autoescaping and the context carries user-controlled values
// (display names, email addresses), so an unescaped name lands as live markup
// in every recipient's mail client.

TEST_F(TemplatesTest, htmlVariantEscapesContextValues) {
    write("xss.html", "<p>Hello {{ user.name }}</p>");
    const std::string payload = "<img src=x onerror=alert(1)>";
    auto out = Email::Templates::render("xss", "html", {{"user", {{"name", payload}}}});
    EXPECT_EQ(out, "<p>Hello &lt;img src=x onerror=alert(1)&gt;</p>");
    // The template's OWN markup must survive — only the interpolated value is
    // escaped, so the mail still renders as HTML.
    EXPECT_NE(out.find("<p>"), std::string::npos) << out;
    EXPECT_EQ(out.find("<img"), std::string::npos) << "injected tag reached the output: " << out;
}

TEST_F(TemplatesTest, textVariantEmitsTheRawValue) {
    write("xss.txt", "Hello {{ user.name }}");
    // Escaping here would show a reader literal "&lt;" in a plain-text mail.
    const std::string payload = R"(<img src=x onerror=alert(1)> & "quoted" 'single')";
    auto out = Email::Templates::render("xss", "txt", {{"user", {{"name", payload}}}});
    EXPECT_EQ(out, "Hello " + payload);
}

TEST_F(TemplatesTest, htmlEscapingCoversQuotesAndAmpersand) {
    // One pass has to be safe for a text node AND for both quoting styles,
    // since the templates interpolate into href="{{ ... }}" too.
    json ctx = {{"link", R"(http://x/?a=1&b=2")"}, {"label", R"(Tom & Jerry's)"}};
    write("attr.html", R"(<a href="{{ link }}">{{ label }}</a>)");
    auto out = Email::Templates::render("attr", "html", ctx);
    EXPECT_EQ(out, R"(<a href="http://x/?a=1&amp;b=2&quot;">Tom &amp; Jerry&#39;s</a>)");
}

TEST_F(TemplatesTest, htmlEscapingReachesNestedAndArrayValues) {
    // Whole-context recursion, not an opt-in key list: a new template variable
    // must be safe by default, however deep it sits.
    json ctx;
    ctx["a"]["b"]["c"] = "<i>";
    ctx["items"] = json::array({"<u>", "plain"});
    write("deep.html", "{{ a.b.c }}|{% for it in items %}{{ it }},{% endfor %}");
    auto out = Email::Templates::render("deep", "html", ctx);
    EXPECT_EQ(out, "&lt;i&gt;|&lt;u&gt;,plain,");
}

TEST_F(TemplatesTest, renderPairEscapesOnlyTheHtmlSide) {
    write("pairxss.txt", "text: {{ v }}");
    write("pairxss.html", "<b>{{ v }}</b>");
    auto pair = Email::Templates::render_pair("pairxss", {{"v", "<img src=x onerror=alert(1)>"}});
    EXPECT_EQ(pair.text, "text: <img src=x onerror=alert(1)>");
    EXPECT_EQ(pair.html, "<b>&lt;img src=x onerror=alert(1)&gt;</b>");
}

TEST_F(TemplatesTest, missingTemplateThrows) {
    EXPECT_THROW(Email::Templates::render("nope", "txt", json::object()), std::runtime_error);
}

TEST_F(TemplatesTest, missingHtmlVariantThrowsFromRenderPair) {
    write("halfpair.txt", "only text");
    EXPECT_THROW(Email::Templates::render_pair("halfpair", json::object()), std::runtime_error);
}

TEST_F(TemplatesTest, defaultContextWithoutConfigHasAppFields) {
    auto ctx = Email::Templates::default_context();
    EXPECT_EQ(ctx["app_name"], "App");
    EXPECT_TRUE(ctx.contains("base_url"));
}

// The repo's real templates: every shipped pair must render against the
// context AccountEmails builds, with the link placeholder substituted.
TEST_F(TemplatesTest, shippedTemplatesRenderWithAccountContext) {
    if (!fs::exists("templates/email"))
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    dir_env_ = std::make_unique<TestHelpers::ScopedEnv>("MAIL_TEMPLATES_DIR", "templates/email");

    json ctx = {{"app_name", "App"},
                {"base_url", "http://x"},
                {"user", {{"email", "a@b.c"}, {"full_name", "A B"}, {"first_name", "A"}, {"last_name", "B"}}},
                {"confirm_link", "http://x/account/confirm/T"},
                {"reset_link", "http://x/account/reset-password/T"},
                {"change_email_link", "http://x/account/change-email/T"},
                {"invite_link", "http://x/account/join-from-invite/T"},
                {"new_email", "n@b.c"}};

    const char* links[][2] = {{"confirm", "http://x/account/confirm/T"},
                              {"reset_password", "http://x/account/reset-password/T"},
                              {"change_email", "http://x/account/change-email/T"},
                              {"invite", "http://x/account/join-from-invite/T"}};
    for (const auto& [name, link] : links) {
        SCOPED_TRACE(name);
        auto pair = Email::Templates::render_pair(name, ctx);
        EXPECT_NE(pair.text.find(link), std::string::npos);
        EXPECT_NE(pair.html.find(link), std::string::npos);
        EXPECT_EQ(pair.text.find("{{"), std::string::npos) << "unsubstituted placeholder in " << name << ".txt";
    }
}

}  // namespace
